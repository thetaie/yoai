// Node-API bridge: Electron's main process <-> the Body Clock C ABI (src/bodyclock.h).
//
// The same bridge harmony/entry/src/main/cpp/napi_init.cpp is, against the same
// header and with the same threading rules, because Node-API and OHOS's NAPI are
// the same API with different registration. Where the two files agree they are
// meant to be read side by side; the differences are marked.
//
// Threading is the whole reason this file exists. bodyclock_chat BLOCKS until the
// turn completes and calls its handler once per event on the calling thread, so
// each turn runs on a detached worker thread and every event crosses back to JS
// through one napi_threadsafe_function. Events arrive in order; the worker
// appends a synthetic terminal event ("end" on success, "aborted" after a
// cancel) because the C ABI signals completion by returning, not by a sentinel.
//
// Cancellation: nativeChatCancel(turnId) flips a per-turn atomic; the handler
// reads it on the next event and returns 0, which tears down the in-flight HTTP
// transfer inside the core. A turn producing no further events cannot be
// interrupted earlier -- there is nothing to observe the flag.
//
// This bridge exposes the chat, on-device and speech-output lanes. The voice
// (recognition) and bundle entry points are in the header and deliberately not
// wired up here: nothing in the desktop client calls them yet, and an export
// nobody uses is a second place to keep correct.

#include <node_api.h>

#include "bodyclock.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------

// Read a JS string argument into a std::string ('' on failure or wrong type).
std::string ToUtf8(napi_env env, napi_value value) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return std::string();

    std::string out(len, '\0');
    if (napi_get_value_string_utf8(env, value, &out[0], len + 1, &len) != napi_ok) return std::string();
    return out;
}

// argv[i] as a string, or '' when the caller passed fewer arguments.
std::string ArgUtf8(napi_env env, size_t argc, napi_value* argv, size_t i) {
    return i < argc ? ToUtf8(env, argv[i]) : std::string();
}

napi_value Int32(napi_env env, std::int32_t v) {
    napi_value out = nullptr;
    napi_create_int32(env, v, &out);
    return out;
}

napi_value Utf8(napi_env env, const char* s) {
    napi_value out = nullptr;
    napi_create_string_utf8(env, s ? s : "", NAPI_AUTO_LENGTH, &out);
    return out;
}

//------------------------------------------------------------------------------
// In-flight turns
//------------------------------------------------------------------------------

// One live turn. Owned jointly by its worker thread and the cancel table
// (shared_ptr), so a cancel racing the turn's end never touches freed memory.
struct Turn {
    std::atomic<bool> cancelled{false};
};

std::mutex g_turns_mutex;
std::map<std::int32_t, std::shared_ptr<Turn>> g_turns;
std::int32_t g_next_turn_id = 1;

// One event crossing the thread boundary; CallJs takes ownership.
struct StreamEvent {
    std::string type;
    std::string content;
};

// Runs ON THE JS THREAD, once per queued event.
void CallJs(napi_env env, napi_value js_callback, void* /*context*/, void* data) {
    std::unique_ptr<StreamEvent> ev(static_cast<StreamEvent*>(data));
    if (env == nullptr || js_callback == nullptr) return;   // env tearing down

    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);

    napi_value argv[2] = {nullptr, nullptr};
    napi_create_string_utf8(env, ev->type.c_str(), NAPI_AUTO_LENGTH, &argv[0]);
    napi_create_string_utf8(env, ev->content.c_str(), NAPI_AUTO_LENGTH, &argv[1]);

    napi_call_function(env, undefined, js_callback, 2, argv, nullptr);
}

// The bodyclock_chat_handler context: where to forward events, and the cancel flag.
struct HandlerContext {
    napi_threadsafe_function tsfn;
    std::shared_ptr<Turn>    turn;
};

// Called once per event on the worker thread. Returning 0 aborts the turn.
int OnChatEvent(const char* event_type, const char* content, void* user_data) {
    HandlerContext* ctx = static_cast<HandlerContext*>(user_data);
    if (ctx->turn->cancelled.load(std::memory_order_relaxed)) return 0;

    StreamEvent* ev = new StreamEvent{event_type ? event_type : "",
                                      content ? content : ""};
    // Blocking, so a slow JS side applies backpressure rather than losing
    // events: a dropped delta would silently corrupt the reply text.
    napi_call_threadsafe_function(ctx->tsfn, ev, napi_tsfn_blocking);
    return 1;
}

// The worker: run the blocking turn, emit the terminal event, release the tsfn.
void ChatWorker(std::int32_t turn_id,
                std::string provider,
                std::string messages_json,
                std::string system_prompt,
                std::string grammar,
                napi_threadsafe_function tsfn,
                std::shared_ptr<Turn> turn) {
    HandlerContext ctx{tsfn, turn};

    const int rc = bodyclock_chat(provider.c_str(), messages_json.c_str(),
                             system_prompt.empty() ? nullptr : system_prompt.c_str(),
                             grammar.empty() ? nullptr : grammar.c_str(),
                             &OnChatEvent, &ctx);

    // A failing turn has ALREADY sent an "error" event carrying the text worth
    // showing, so the terminal event only has to say how it ended. Cancel is
    // distinguished from failure because the UI treats them differently: one is
    // the user's own doing, the other needs reporting.
    const bool cancelled = turn->cancelled.load(std::memory_order_relaxed);
    if (rc != 0 && !cancelled) {
        std::fprintf(stderr, "[bodyclock] chat turn %d finished rc=%d\n", turn_id, rc);
    }

    StreamEvent* fin = new StreamEvent{cancelled ? "aborted" : "end", ""};
    napi_call_threadsafe_function(tsfn, fin, napi_tsfn_blocking);

    // Releasing the last reference unrefs the event loop, so a process that is
    // only waiting on this turn can now exit. Until then the loop stays alive,
    // which is why quitting mid-turn goes through nativeChatCancelAll.
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);

    std::lock_guard<std::mutex> lock(g_turns_mutex);
    g_turns.erase(turn_id);
}

//------------------------------------------------------------------------------
// Exported functions
//------------------------------------------------------------------------------

// nativeVersion(): string -- build stamp, proves the .node is really loaded.
napi_value NativeVersion(napi_env env, napi_callback_info /*info*/) {
    return Utf8(env, ("bodyclock " + std::string(bodyclock_version())
                      + " / built " __DATE__ " " __TIME__).c_str());
}

// nativeSetLogLevel(level: string): void
napi_value NativeSetLogLevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string level = ArgUtf8(env, argc, argv, 0);
    bodyclock_set_log_level(level.c_str());
    return nullptr;
}

// nativeProviderSet(id, baseUrl, apiKey, model, extraJson): number  (0 = ok)
napi_value NativeProviderSet(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    const std::string id       = ArgUtf8(env, argc, argv, 0);
    const std::string base_url = ArgUtf8(env, argc, argv, 1);
    const std::string api_key  = ArgUtf8(env, argc, argv, 2);
    const std::string model    = ArgUtf8(env, argc, argv, 3);
    const std::string extra    = ArgUtf8(env, argc, argv, 4);

    return Int32(env, bodyclock_provider_set(id.c_str(), base_url.c_str(), api_key.c_str(),
                                        model.c_str(), extra.empty() ? nullptr : extra.c_str()));
}

// nativeProviderRemove(id: string): boolean
napi_value NativeProviderRemove(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    const std::string id = ArgUtf8(env, argc, argv, 0);
    napi_value out = nullptr;
    napi_get_boolean(env, bodyclock_provider_remove(id.c_str()) != 0, &out);
    return out;
}

// nativeProviderClear(): void
napi_value NativeProviderClear(napi_env /*env*/, napi_callback_info /*info*/) {
    bodyclock_provider_clear();
    return nullptr;
}

// nativeProviders(): string  (newline-separated ids; '' when none)
napi_value NativeProviders(napi_env env, napi_callback_info /*info*/) {
    return Utf8(env, bodyclock_providers());
}

//------------------------------------------------------------------------------
// On-device models
//------------------------------------------------------------------------------

// nativeLocalAvailable(): boolean -- was this .node built with llama.cpp?
napi_value NativeLocalAvailable(napi_env env, napi_callback_info /*info*/) {
    napi_value out = nullptr;
    napi_get_boolean(env, bodyclock_local_available() != 0, &out);
    return out;
}

// nativeLocalSet(id, modelPath, optionsJson): number  (0 = ok)
napi_value NativeLocalSet(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    const std::string id      = ArgUtf8(env, argc, argv, 0);
    const std::string path    = ArgUtf8(env, argc, argv, 1);
    const std::string options = ArgUtf8(env, argc, argv, 2);

    return Int32(env, bodyclock_local_set(id.c_str(), path.c_str(),
                                     options.empty() ? nullptr : options.c_str()));
}

// The worker for nativeLocalLoad: read the weights, then report once and let go.
//
// A separate thread for the same reason a turn gets one -- bodyclock_local_load
// blocks for seconds reading gigabytes, and doing that on the main process's
// thread freezes every window at exactly the moment the user is being told the
// model is getting ready.
void LoadWorker(std::string id, napi_threadsafe_function tsfn) {
    const int rc = bodyclock_local_load(id.c_str());

    // Reuses the chat callback's (type, content) shape rather than inventing a
    // second one: the JS side already has a handler of that signature, and one
    // vocabulary is easier to hold than two.
    StreamEvent* ev = new StreamEvent{rc == 0 ? "end" : "error",
                                      rc == 0 ? "" : "could not load the model"};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
}

// nativeLocalLoad(id, onDone: (type, content) => void): void
//
// Not a turn, so it takes no turn id and cannot be cancelled: a load either
// finishes or fails, and abandoning one halfway leaves the file partly read for
// no benefit. The callback fires exactly once.
napi_value NativeLocalLoad(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_error(env, nullptr, "nativeLocalLoad(id, onDone) needs two arguments");
        return nullptr;
    }

    std::string id = ToUtf8(env, argv[0]);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "bodyclock_local_load", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[1], nullptr, work_name,
                                        0 /*unbounded*/, 1 /*one worker*/,
                                        nullptr, nullptr, nullptr, CallJs, &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    std::thread(LoadWorker, std::move(id), tsfn).detach();
    return nullptr;
}

// nativeLocalSetThreads(id: string, threads: number): number
//
// Synchronous and cheap -- it sets a field the next turn reads, and does not
// touch the weights.
napi_value NativeLocalSetThreads(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_error(env, nullptr,
                         "nativeLocalSetThreads(id, threads) needs two arguments");
        return nullptr;
    }
    std::string id = ToUtf8(env, argv[0]);
    std::int32_t n = 0;
    napi_get_value_int32(env, argv[1], &n);
    return Int32(env, bodyclock_local_set_threads(id.c_str(), n));
}

//------------------------------------------------------------------------------
// Chat
//------------------------------------------------------------------------------

// nativeChat(provider, messagesJson, systemPrompt, grammar,
//            onEvent: (type, content) => void): number   (turn id)
napi_value NativeChat(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 5) {
        napi_throw_error(env, nullptr,
                         "nativeChat(provider, messagesJson, systemPrompt, grammar, onEvent) "
                         "needs five arguments");
        return nullptr;
    }

    std::string provider = ToUtf8(env, argv[0]);
    std::string messages = ToUtf8(env, argv[1]);
    std::string system   = ToUtf8(env, argv[2]);
    std::string grammar  = ToUtf8(env, argv[3]);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "bodyclock_chat", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[4], nullptr, work_name,
                                        0 /*unbounded*/, 1 /*one worker*/,
                                        nullptr, nullptr, nullptr, CallJs, &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    std::shared_ptr<Turn> turn(new Turn());
    std::int32_t id;
    {
        std::lock_guard<std::mutex> lock(g_turns_mutex);
        id = g_next_turn_id++;
        g_turns[id] = turn;
    }

    std::thread(ChatWorker, id, std::move(provider), std::move(messages),
                std::move(system), std::move(grammar), tsfn, turn).detach();

    return Int32(env, id);
}

// nativeChatCancel(turnId: number): void
napi_value NativeChatCancel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    std::int32_t id = 0;
    if (argc > 0) napi_get_value_int32(env, argv[0], &id);

    std::lock_guard<std::mutex> lock(g_turns_mutex);
    auto it = g_turns.find(id);
    if (it != g_turns.end()) {
        it->second->cancelled.store(true, std::memory_order_relaxed);
    }
    return nullptr;
}

// nativeChatCancelAll(): number  (how many turns were asked to stop)
//
// NOT in the HarmonyOS bridge, and the difference is the host rather than the
// core: a phone app is killed by the system, while a desktop app is asked to
// quit and has to let go by itself. Every in-flight turn holds a threadsafe
// function, which keeps the main process's event loop alive, so quitting during
// a long reply would hang until the provider finished answering a window that
// is already gone.
//
// Asks only -- the turn ends when its next event arrives and the handler
// returns 0, so a caller that is quitting should still wait for the "aborted"
// events rather than assume this was instant.
napi_value NativeChatCancelAll(napi_env env, napi_callback_info /*info*/) {
    std::lock_guard<std::mutex> lock(g_turns_mutex);
    for (auto& entry : g_turns) {
        entry.second->cancelled.store(true, std::memory_order_relaxed);
    }
    return Int32(env, static_cast<std::int32_t>(g_turns.size()));
}

//------------------------------------------------------------------------------
// Speech output (qwentts.cpp)
//------------------------------------------------------------------------------
//
// No #ifdef, and that is the header's doing rather than an omission: the
// bodyclock_tts_* entry points are compiled unconditionally and stub out when
// the qtts lane is off, so a `build.cmd llama` addon still exports all of this
// and answers nativeTtsAvailable() with 0. The UI asks that question instead of
// discovering a missing symbol.
//
// Audio crosses the thread boundary the same way chat deltas do -- one
// threadsafe function, blocking calls, events in order -- with one difference
// that earns its own event struct: a chunk is PCM, not text. CallJsTts hands
// "audio" to JS as a Float32Array over a copy, and everything else as a string.
//
// Rate is 24 kHz mono, which the JS side has to know because nothing in the
// samples says so. It is stated once here and once in shared/api.ts.

// One TTS event crossing to the JS thread; CallJsTts takes ownership.
struct TtsEvent {
    std::string        type;      // "audio" | "end" | "error"
    std::string        content;   // stats JSON, or the error. Empty for audio.
    std::vector<float> pcm;       // 24 kHz mono, only for "audio".
};

// At most one utterance at a time. The core serialises on its own mutex, so a
// second say would not corrupt anything -- it would queue, and the user would
// hear the previous reply start after they asked for this one, which is worse
// than being told no.
std::atomic<bool> g_tts_busy{false};
std::atomic<bool> g_tts_cancel{false};

// Runs ON THE JS THREAD, once per queued event.
void CallJsTts(napi_env env, napi_value js_callback, void* /*context*/, void* data) {
    std::unique_ptr<TtsEvent> ev(static_cast<TtsEvent*>(data));
    if (env == nullptr || js_callback == nullptr) return;   // env tearing down

    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);

    napi_value argv[2] = {nullptr, nullptr};
    napi_create_string_utf8(env, ev->type.c_str(), NAPI_AUTO_LENGTH, &argv[0]);

    if (ev->type == "audio") {
        // A copy into an ArrayBuffer rather than an external one over the
        // vector: an external buffer would have to outlive whatever JS does
        // with it, and the finalizer that would guarantee that costs more than
        // the memcpy it saves.
        const size_t count = ev->pcm.size();
        const size_t bytes = count * sizeof(float);
        void*        raw   = nullptr;
        napi_value   ab    = nullptr;
        if (napi_create_arraybuffer(env, bytes, &raw, &ab) == napi_ok) {
            if (bytes > 0) std::memcpy(raw, ev->pcm.data(), bytes);
            napi_create_typedarray(env, napi_float32_array, count, ab, 0, &argv[1]);
        } else {
            napi_get_undefined(env, &argv[1]);
        }
    } else {
        napi_create_string_utf8(env, ev->content.c_str(), NAPI_AUTO_LENGTH, &argv[1]);
    }

    napi_call_function(env, undefined, js_callback, 2, argv, nullptr);
}

// Called by the engine on the worker thread, once per chunk. 0 aborts.
//
// `last` arrives on a call of its own with n_samples 0 -- the header says so --
// and is dropped here: the worker emits "end" with the run's statistics once
// bodyclock_tts_say returns, which is strictly later and carries more.
int OnTtsAudio(const float* pcm, int n_samples, int /*last*/, void* user_data) {
    if (g_tts_cancel.load(std::memory_order_relaxed)) return 0;
    if (pcm == nullptr || n_samples <= 0) return 1;

    napi_threadsafe_function tsfn = static_cast<napi_threadsafe_function>(user_data);
    TtsEvent* ev = new TtsEvent{"audio", "", std::vector<float>(pcm, pcm + n_samples)};
    // Blocking, so a slow JS side applies backpressure rather than dropping a
    // chunk -- a dropped chunk is a gap in the middle of a word.
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    return 1;
}

void TtsLoadWorker(std::string talker, std::string codec, napi_threadsafe_function tsfn) {
    const int rc = bodyclock_tts_load(talker.c_str(), codec.c_str());
    TtsEvent* ev = new TtsEvent{rc == 0 ? "end" : "error",
                                rc == 0 ? "" : bodyclock_tts_last_error(),
                                {}};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
}

void TtsSayWorker(std::string text, napi_threadsafe_function tsfn) {
    const char* stats = bodyclock_tts_say(text.c_str(), OnTtsAudio, tsfn);

    TtsEvent* ev = stats != nullptr
                       ? new TtsEvent{"end", stats, {}}
                       : new TtsEvent{"error", bodyclock_tts_last_error(), {}};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);

    g_tts_cancel.store(false, std::memory_order_relaxed);
    g_tts_busy.store(false, std::memory_order_relaxed);
}

// nativeTtsAvailable(): number -- 1 when this build has the engine.
napi_value NativeTtsAvailable(napi_env env, napi_callback_info /*info*/) {
    return Int32(env, bodyclock_tts_available());
}

// nativeTtsLoad(talkerPath, codecPath, onDone: (type, content) => void): void
//
// On a worker for the same reason nativeLocalLoad is: reading two GGUFs blocks
// for seconds, and doing it on the main thread freezes every window at the
// moment the user is being told the voice is getting ready.
napi_value NativeTtsLoad(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 3) {
        napi_throw_error(env, nullptr, "nativeTtsLoad(talker, codec, onDone) needs three arguments");
        return nullptr;
    }

    std::string talker = ToUtf8(env, argv[0]);
    std::string codec  = ToUtf8(env, argv[1]);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "bodyclock_tts_load", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[2], nullptr, work_name,
                                        0 /*unbounded*/, 1 /*one worker*/,
                                        nullptr, nullptr, nullptr, CallJsTts, &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    std::thread(TtsLoadWorker, std::move(talker), std::move(codec), tsfn).detach();
    return nullptr;
}

napi_value NativeTtsLoaded(napi_env env, napi_callback_info /*info*/) {
    return Int32(env, bodyclock_tts_loaded());
}

napi_value NativeTtsUnload(napi_env env, napi_callback_info /*info*/) {
    bodyclock_tts_unload();
    return nullptr;
}

napi_value NativeTtsModelType(napi_env env, napi_callback_info /*info*/) {
    return Utf8(env, bodyclock_tts_model_type());
}

napi_value NativeTtsSpeakers(napi_env env, napi_callback_info /*info*/) {
    return Utf8(env, bodyclock_tts_speakers());
}

napi_value NativeTtsSetSpeaker(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string name = ArgUtf8(env, argc, argv, 0);
    bodyclock_tts_set_speaker(name.c_str());
    return nullptr;
}

napi_value NativeTtsSetVoice(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string instruct = ArgUtf8(env, argc, argv, 0);
    bodyclock_tts_set_voice(instruct.c_str());
    return nullptr;
}

napi_value NativeTtsSetSeed(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int64_t seed = 0;
    if (argc >= 1) napi_get_value_int64(env, argv[0], &seed);
    bodyclock_tts_set_seed(static_cast<long long>(seed));
    return nullptr;
}

napi_value NativeTtsSetStreaming(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t on = 0;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &on);
    bodyclock_tts_set_streaming(on);
    return nullptr;
}

napi_value NativeTtsSetThreads(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t threads = 0;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &threads);
    bodyclock_tts_set_threads(threads);
    return nullptr;
}

// nativeTtsSay(text, onEvent: (type, payload) => void): number
//
// 1 when the utterance started, 0 when one is already running. Returning a
// status rather than throwing because "busy" is a normal thing for a UI to ask
// and get told no about.
napi_value NativeTtsSay(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_error(env, nullptr, "nativeTtsSay(text, onEvent) needs two arguments");
        return nullptr;
    }

    bool expected = false;
    if (!g_tts_busy.compare_exchange_strong(expected, true)) {
        return Int32(env, 0);
    }
    g_tts_cancel.store(false, std::memory_order_relaxed);

    std::string text = ToUtf8(env, argv[0]);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "bodyclock_tts_say", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[1], nullptr, work_name,
                                        0 /*unbounded*/, 1 /*one worker*/,
                                        nullptr, nullptr, nullptr, CallJsTts, &tsfn) != napi_ok) {
        g_tts_busy.store(false, std::memory_order_relaxed);
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    std::thread(TtsSayWorker, std::move(text), tsfn).detach();
    return Int32(env, 1);
}

// Asks the running utterance to stop. Like a chat cancel, it takes effect at
// the next chunk -- an utterance producing no further audio cannot be
// interrupted earlier, because there is nothing to observe the flag.
napi_value NativeTtsCancel(napi_env env, napi_callback_info /*info*/) {
    g_tts_cancel.store(true, std::memory_order_relaxed);
    return Int32(env, g_tts_busy.load(std::memory_order_relaxed) ? 1 : 0);
}

napi_value NativeTtsLastError(napi_env env, napi_callback_info /*info*/) {
    return Utf8(env, bodyclock_tts_last_error());
}

}   // namespace

//------------------------------------------------------------------------------
// Module registration
//------------------------------------------------------------------------------
//
// The one structural difference from the HarmonyOS bridge. There a napi_module
// is registered from a library constructor; here Node calls the init function
// when require() loads the .node, which is what NAPI_MODULE_INIT expands to.

NAPI_MODULE_INIT() {
    napi_property_descriptor desc[] = {
        {"nativeVersion",         nullptr, NativeVersion,         nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetLogLevel",     nullptr, NativeSetLogLevel,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeProviderSet",     nullptr, NativeProviderSet,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeProviderRemove",  nullptr, NativeProviderRemove,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeProviderClear",   nullptr, NativeProviderClear,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeProviders",       nullptr, NativeProviders,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeLocalAvailable",  nullptr, NativeLocalAvailable,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeLocalSet",        nullptr, NativeLocalSet,        nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeLocalLoad",       nullptr, NativeLocalLoad,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeLocalSetThreads", nullptr, NativeLocalSetThreads, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeChat",            nullptr, NativeChat,            nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeChatCancel",      nullptr, NativeChatCancel,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeChatCancelAll",   nullptr, NativeChatCancelAll,   nullptr, nullptr, nullptr, napi_default, nullptr},

        {"nativeTtsAvailable",    nullptr, NativeTtsAvailable,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsLoad",         nullptr, NativeTtsLoad,         nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsLoaded",       nullptr, NativeTtsLoaded,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsUnload",       nullptr, NativeTtsUnload,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsModelType",    nullptr, NativeTtsModelType,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSpeakers",     nullptr, NativeTtsSpeakers,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSetSpeaker",   nullptr, NativeTtsSetSpeaker,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSetVoice",     nullptr, NativeTtsSetVoice,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSetSeed",      nullptr, NativeTtsSetSeed,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSetStreaming", nullptr, NativeTtsSetStreaming, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSetThreads",   nullptr, NativeTtsSetThreads,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSay",          nullptr, NativeTtsSay,          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsCancel",       nullptr, NativeTtsCancel,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsLastError",    nullptr, NativeTtsLastError,    nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
