// Node-API bridge: Electron's main process <-> the YoAI C ABI (src/yoai.h).
//
// The same bridge examples/harmony/entry/src/main/cpp/napi_init.cpp is, against the same
// header and with the same threading rules, because Node-API and OHOS's NAPI are
// the same API with different registration. Where the two files agree they are
// meant to be read side by side; the differences are marked.
//
// Threading is the whole reason this file exists. yoai_chat BLOCKS until the
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
// This bridge exposes the chat, prompt, on-device, speech-output and omni
// lanes. The voice (recognition) and bundle entry points are in the header and
// deliberately not wired up here: nothing in the desktop client calls them yet,
// and an export nobody uses is a second place to keep correct.

#include <node_api.h>

#include "yoai.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
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
// Worker threads
//------------------------------------------------------------------------------

// Every call into the core that blocks runs on a detached worker, and this
// counts them: up when one is launched, down when its call has returned. The
// main process reads it through nativeWorkers() on quit, and unloads the
// engines only once it is 0 -- unloading under a running worker frees what
// that worker is using.
std::atomic<int> g_workers{0};

template <typename Fn, typename... Args>
void RunCounted(Fn fn, Args... args) {
    fn(std::move(args)...);
    g_workers.fetch_sub(1, std::memory_order_relaxed);
}

// std::thread(fn, args...).detach(), counted in g_workers. Counted here, on
// the JS thread, rather than by the worker, so a read right after a launch
// already sees it.
template <typename Fn, typename... Args>
void Launch(Fn fn, Args&&... args) {
    g_workers.fetch_add(1, std::memory_order_relaxed);
    std::thread(&RunCounted<Fn, typename std::decay<Args>::type...>, fn,
                std::forward<Args>(args)...).detach();
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

// The yoai_chat_handler context: where to forward events, and the cancel flag.
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

    const int rc = yoai_chat(provider.c_str(), messages_json.c_str(),
                             system_prompt.empty() ? nullptr : system_prompt.c_str(),
                             grammar.empty() ? nullptr : grammar.c_str(),
                             &OnChatEvent, &ctx);

    // A failing turn has ALREADY sent an "error" event carrying the text worth
    // showing, so the terminal event only has to say how it ended. Cancel is
    // distinguished from failure because the UI treats them differently: one is
    // the user's own doing, the other needs reporting.
    const bool cancelled = turn->cancelled.load(std::memory_order_relaxed);
    if (rc != 0 && !cancelled) {
        std::fprintf(stderr, "[yoai] chat turn %d finished rc=%d\n", turn_id, rc);
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
    return Utf8(env, ("yoai " + std::string(yoai_version())
                      + " / built " __DATE__ " " __TIME__).c_str());
}

// nativeSetLogLevel(level: string): void
napi_value NativeSetLogLevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string level = ArgUtf8(env, argc, argv, 0);
    yoai_set_log_level(level.c_str());
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

    return Int32(env, yoai_provider_set(id.c_str(), base_url.c_str(), api_key.c_str(),
                                        model.c_str(), extra.empty() ? nullptr : extra.c_str()));
}

// nativeProviderRemove(id: string): boolean
napi_value NativeProviderRemove(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    const std::string id = ArgUtf8(env, argc, argv, 0);
    napi_value out = nullptr;
    napi_get_boolean(env, yoai_provider_remove(id.c_str()) != 0, &out);
    return out;
}

// nativeProviderClear(): void
napi_value NativeProviderClear(napi_env /*env*/, napi_callback_info /*info*/) {
    yoai_provider_clear();
    return nullptr;
}

// nativeProviders(): string  (newline-separated ids; '' when none)
napi_value NativeProviders(napi_env env, napi_callback_info /*info*/) {
    return Utf8(env, yoai_providers());
}

//------------------------------------------------------------------------------
// On-device models
//------------------------------------------------------------------------------

// nativeLocalAvailable(): boolean -- was this .node built with llama.cpp?
napi_value NativeLocalAvailable(napi_env env, napi_callback_info /*info*/) {
    napi_value out = nullptr;
    napi_get_boolean(env, yoai_local_available() != 0, &out);
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

    return Int32(env, yoai_local_set(id.c_str(), path.c_str(),
                                     options.empty() ? nullptr : options.c_str()));
}

// The worker for nativeLocalLoad: read the weights, then report once and let go.
//
// A separate thread for the same reason a turn gets one -- yoai_local_load
// blocks for seconds reading gigabytes, and doing that on the main process's
// thread freezes every window at exactly the moment the user is being told the
// model is getting ready.
void LoadWorker(std::string id, napi_threadsafe_function tsfn) {
    const int rc = yoai_local_load(id.c_str());

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
    napi_create_string_utf8(env, "yoai_local_load", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[1], nullptr, work_name,
                                        0 /*unbounded*/, 1 /*one worker*/,
                                        nullptr, nullptr, nullptr, CallJs, &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    Launch(LoadWorker, std::move(id), tsfn);
    return nullptr;
}

void WarmWorker(std::string id, std::string prompt, napi_threadsafe_function tsfn) {
    const int rc = yoai_local_warm(id.c_str(), prompt.c_str());
    StreamEvent* ev = new StreamEvent{rc == 0 ? "end" : "error",
                                      rc == 0 ? "" : "could not warm the prompt"};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
}

// nativeLocalWarm(id, systemPrompt, onDone: (type, content) => void): void
//
// On a worker like a load: it prefills the system prompt, seconds on a long
// one, and loads the model first if it is not resident. Fires once.
napi_value NativeLocalWarm(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 3) {
        napi_throw_error(env, nullptr, "nativeLocalWarm(id, systemPrompt, onDone) needs three arguments");
        return nullptr;
    }
    std::string id     = ToUtf8(env, argv[0]);
    std::string prompt = ToUtf8(env, argv[1]);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "yoai_local_warm", NAPI_AUTO_LENGTH, &work_name);
    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[2], nullptr, work_name, 0, 1,
                                        nullptr, nullptr, nullptr, CallJs, &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }
    Launch(WarmWorker, std::move(id), std::move(prompt), tsfn);
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
    return Int32(env, yoai_local_set_threads(id.c_str(), n));
}

// nativeLocalForget(id: string): number  (0 = ok)
//
// Synchronous and cheap for the same reason: it drops a cache and leaves the
// weights alone. What it is for is in yoai.h -- a new conversation is an
// instruction, not something to be inferred from a shorter prompt.
napi_value NativeLocalForget(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string id = ArgUtf8(env, argc, argv, 0);
    return Int32(env, yoai_local_forget(id.c_str()));
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
    napi_create_string_utf8(env, "yoai_chat", NAPI_AUTO_LENGTH, &work_name);

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

    Launch(ChatWorker, id, std::move(provider), std::move(messages),
           std::move(system), std::move(grammar), tsfn, turn);

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

// nativeWorkers(): number -- calls into the core still running on a worker
// thread, of every kind: turns, loads, warm-ups, reading. See g_workers.
napi_value NativeWorkers(napi_env env, napi_callback_info /*info*/) {
    return Int32(env, g_workers.load(std::memory_order_relaxed));
}

//------------------------------------------------------------------------------
// Prompts
//------------------------------------------------------------------------------
//
// The two the chat needs. The defaults live in the core, so this side keeps no
// copy: nativePromptGet shows the default on the settings screen, and
// nativePromptTurnSystem builds what a turn and its warm-up send.

// nativePromptGet(name: string): string | null  (null for an unknown name)
napi_value NativePromptGet(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string name = ArgUtf8(env, argc, argv, 0);

    const char* text = yoai_prompt_get(name.c_str(), nullptr);
    if (text == nullptr) {
        napi_value null_value = nullptr;
        napi_get_null(env, &null_value);
        return null_value;
    }
    return Utf8(env, text);
}

// nativePromptTurnSystem(base: string | null, reading: boolean): string
//
// Anything but a string is a user who never wrote a prompt, and gets the core's
// default; '' is one who cleared it, and gets none.
napi_value NativePromptTurnSystem(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    napi_valuetype type = napi_undefined;
    if (argc >= 1) napi_typeof(env, argv[0], &type);
    const bool        given = type == napi_string;
    const std::string base  = given ? ToUtf8(env, argv[0]) : std::string();

    bool reading = false;
    if (argc >= 2) napi_get_value_bool(env, argv[1], &reading);

    return Utf8(env, yoai_prompt_turn_system(given ? base.c_str() : nullptr, reading ? 1 : 0));
}

//------------------------------------------------------------------------------
// Speech output (qwentts.cpp)
//------------------------------------------------------------------------------
//
// No #ifdef, and that is the header's doing rather than an omission: the
// yoai_tts_* entry points are compiled unconditionally and stub out when
// the qtts lane is off, so a `build.py llama` addon still exports all of this
// and answers nativeTtsAvailable() with 0. The UI asks that question instead of
// discovering a missing symbol.
//
// Audio crosses the thread boundary the same way chat deltas do -- one
// threadsafe function, blocking calls, events in order -- with one difference
// that earns its own event struct: a chunk is PCM, not text. CallJsAudio hands
// "audio" to JS as a Float32Array over a copy, and everything else as a string.
//
// Rate is 24 kHz mono, which the JS side has to know because nothing in the
// samples says so. It is stated once here and once in shared/api.ts.

// One audio-lane event crossing to the JS thread; CallJsAudio takes ownership.
// Shared by speech output and by omni: both hand back 24 kHz mono float in
// chunks and a JSON summary at the end, so one shape serves both rather than
// two that would have to be kept identical.
struct AudioEvent {
    std::string        type;      // "audio" | "text" | "end" | "error"
    std::string        content;   // the reply, stats JSON, or the error. Empty for audio.
    std::vector<float> pcm;       // 24 kHz mono, only for "audio".
    // Which piece these samples belong to, for the reading lane. -1 on every
    // event the other two produce, and CallJsAudio then passes two arguments
    // exactly as before -- so this field costs their callbacks nothing.
    int                sentence;

    // Spelled out because C++11 does not let a struct with a default member
    // initializer be brace-initialized as an aggregate; this keeps every
    // AudioEvent{...} below working and the addon on the core's C++11.
    AudioEvent(std::string t, std::string c, std::vector<float> p, int s = -1)
        : type(std::move(t)), content(std::move(c)), pcm(std::move(p)), sentence(s) {}
};

// At most one utterance at a time. The core serialises on its own mutex, so a
// second say would not corrupt anything -- it would queue, and the user would
// hear the previous reply start after they asked for this one, which is worse
// than being told no.
std::atomic<bool> g_tts_busy{false};
std::atomic<bool> g_tts_cancel{false};

// Runs ON THE JS THREAD, once per queued event.
void CallJsAudio(napi_env env, napi_value js_callback, void* /*context*/, void* data) {
    std::unique_ptr<AudioEvent> ev(static_cast<AudioEvent*>(data));
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

    // A third argument only when there is one to give, so the speech and omni
    // lanes' callbacks keep the arity they were written against.
    if (ev->sentence >= 0) {
        napi_value argv3[3] = {argv[0], argv[1], nullptr};
        napi_create_int32(env, ev->sentence, &argv3[2]);
        napi_call_function(env, undefined, js_callback, 3, argv3, nullptr);
        return;
    }
    napi_call_function(env, undefined, js_callback, 2, argv, nullptr);
}

// Called by the engine on the worker thread, once per chunk. 0 aborts.
//
// `last` arrives on a call of its own with n_samples 0 -- the header says so --
// and is dropped here: the worker emits "end" with the run's statistics once
// yoai_tts_say returns, which is strictly later and carries more.
int OnTtsAudio(const float* pcm, int n_samples, int /*last*/, void* user_data) {
    if (g_tts_cancel.load(std::memory_order_relaxed)) return 0;
    if (pcm == nullptr || n_samples <= 0) return 1;

    napi_threadsafe_function tsfn = static_cast<napi_threadsafe_function>(user_data);
    AudioEvent* ev = new AudioEvent{"audio", "", std::vector<float>(pcm, pcm + n_samples)};
    // Blocking, so a slow JS side applies backpressure rather than dropping a
    // chunk -- a dropped chunk is a gap in the middle of a word.
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    return 1;
}

void TtsLoadWorker(std::string talker, std::string codec, napi_threadsafe_function tsfn) {
    const int rc = yoai_tts_load(talker.c_str(), codec.c_str());
    AudioEvent* ev = new AudioEvent{rc == 0 ? "end" : "error",
                                rc == 0 ? "" : yoai_tts_last_error(),
                                {}};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
}

void TtsSayWorker(std::string text, napi_threadsafe_function tsfn) {
    const char* stats = yoai_tts_say(text.c_str(), OnTtsAudio, tsfn);

    AudioEvent* ev = stats != nullptr
                       ? new AudioEvent{"end", stats, {}}
                       : new AudioEvent{"error", yoai_tts_last_error(), {}};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);

    g_tts_cancel.store(false, std::memory_order_relaxed);
    g_tts_busy.store(false, std::memory_order_relaxed);
}

// nativeTtsAvailable(): number -- 1 when this build has the engine.
napi_value NativeTtsAvailable(napi_env env, napi_callback_info /*info*/) {
    return Int32(env, yoai_tts_available());
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
    napi_create_string_utf8(env, "yoai_tts_load", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[2], nullptr, work_name,
                                        0 /*unbounded*/, 1 /*one worker*/,
                                        nullptr, nullptr, nullptr, CallJsAudio, &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    Launch(TtsLoadWorker, std::move(talker), std::move(codec), tsfn);
    return nullptr;
}

napi_value NativeTtsLoaded(napi_env env, napi_callback_info /*info*/) {
    return Int32(env, yoai_tts_loaded());
}

napi_value NativeTtsUnload(napi_env env, napi_callback_info /*info*/) {
    yoai_tts_unload();
    return nullptr;
}

napi_value NativeTtsModelType(napi_env env, napi_callback_info /*info*/) {
    return Utf8(env, yoai_tts_model_type());
}

napi_value NativeTtsSpeakers(napi_env env, napi_callback_info /*info*/) {
    return Utf8(env, yoai_tts_speakers());
}

napi_value NativeTtsSetSpeaker(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string name = ArgUtf8(env, argc, argv, 0);
    yoai_tts_set_speaker(name.c_str());
    return nullptr;
}

napi_value NativeTtsSetVoice(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string instruct = ArgUtf8(env, argc, argv, 0);
    yoai_tts_set_voice(instruct.c_str());
    return nullptr;
}

napi_value NativeTtsSetSeed(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int64_t seed = 0;
    if (argc >= 1) napi_get_value_int64(env, argv[0], &seed);
    yoai_tts_set_seed(static_cast<long long>(seed));
    return nullptr;
}

napi_value NativeTtsSetStreaming(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t on = 0;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &on);
    yoai_tts_set_streaming(on);
    return nullptr;
}

napi_value NativeTtsSetThreads(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t threads = 0;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &threads);
    yoai_tts_set_threads(threads);
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
    napi_create_string_utf8(env, "yoai_tts_say", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[1], nullptr, work_name,
                                        0 /*unbounded*/, 1 /*one worker*/,
                                        nullptr, nullptr, nullptr, CallJsAudio, &tsfn) != napi_ok) {
        g_tts_busy.store(false, std::memory_order_relaxed);
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    Launch(TtsSayWorker, std::move(text), tsfn);
    return Int32(env, 1);
}

// Asks the running utterance to stop. Like a chat cancel, it takes effect at
// the next chunk -- an utterance producing no further audio cannot be
// interrupted earlier, because there is nothing to observe the flag.
napi_value NativeTtsCancel(napi_env env, napi_callback_info /*info*/) {
    g_tts_cancel.store(true, std::memory_order_relaxed);
    return Int32(env, g_tts_busy.load(std::memory_order_relaxed) ? 1 : 0);
}

//------------------------------------------------------------------------------
// Reading a reply aloud while it is still being written
//------------------------------------------------------------------------------
//
// A second engine over the same weights as the speech lane, so a host loads one
// or the other -- see yoai.h. It reuses the audio machinery above because
// the output side is identical: 24 kHz mono float in chunks, then a JSON
// summary. What is new is the INPUT: text arrives a token at a time, and the
// call that ends the reply blocks for the rest of the speech.

// Called by the engines on their own threads, once per block, in reading order.
int OnReaderAudio(const float* pcm, int n_samples, int sentence, int /*last*/,
                  void* user_data) {
    if (pcm == nullptr || n_samples <= 0) return 1;
    napi_threadsafe_function tsfn = static_cast<napi_threadsafe_function>(user_data);
    AudioEvent* ev = new AudioEvent{"audio", "", std::vector<float>(pcm, pcm + n_samples),
                                    sentence};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    return 1;
}

void ReaderLoadWorker(std::string talker, std::string codec, int voices,
                      napi_threadsafe_function tsfn) {
    const int rc = yoai_reader_load(talker.c_str(), codec.c_str(), voices);
    AudioEvent* ev = new AudioEvent{rc == 0 ? "end" : "error",
                                    rc == 0 ? "" : yoai_reader_last_error(),
                                    {}, -1};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
}

// **On a worker, because finish() blocks for the rest of the speech.** The
// audio channel is released here rather than by the caller: this is the only
// place that knows the stream is over.
void ReaderFinishWorker(napi_threadsafe_function audio_tsfn,
                        napi_threadsafe_function done_tsfn) {
    const char* stats = yoai_reader_finish();
    AudioEvent* ev = stats != nullptr
                         ? new AudioEvent{"end", stats, {}, -1}
                         : new AudioEvent{"error", yoai_reader_last_error(), {}, -1};
    napi_call_threadsafe_function(done_tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(done_tsfn, napi_tsfn_release);
    if (audio_tsfn != nullptr) {
        napi_release_threadsafe_function(audio_tsfn, napi_tsfn_release);
    }
}

// Held between begin() and finish(): the audio channel outlives the call that
// created it, which is the whole difference from nativeTtsSay.
napi_threadsafe_function g_reader_audio = nullptr;

napi_value NativeReaderAvailable(napi_env env, napi_callback_info /*info*/) {
    return Int32(env, yoai_reader_available());
}

// nativeReaderLoad(talker, codec, voices, onDone: (type, content) => void): void
napi_value NativeReaderLoad(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        napi_throw_error(env, nullptr,
                         "nativeReaderLoad(talker, codec, voices, onDone) needs four arguments");
        return nullptr;
    }

    std::string talker = ToUtf8(env, argv[0]);
    std::string codec  = ToUtf8(env, argv[1]);
    std::int32_t voices = 1;
    napi_get_value_int32(env, argv[2], &voices);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "yoai_reader_load", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[3], nullptr, work_name, 0, 1,
                                        nullptr, nullptr, nullptr, CallJsAudio,
                                        &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }
    Launch(ReaderLoadWorker, std::move(talker), std::move(codec),
           static_cast<int>(voices), tsfn);
    return nullptr;
}

napi_value NativeReaderLoaded(napi_env env, napi_callback_info /*info*/) {
    return Int32(env, yoai_reader_loaded());
}

napi_value NativeReaderUnload(napi_env env, napi_callback_info /*info*/) {
    yoai_reader_unload();
    return nullptr;
}

napi_value NativeReaderModelType(napi_env env, napi_callback_info /*info*/) {
    return Utf8(env, yoai_reader_model_type());
}

// nativeReaderSpeakers(): string, a JSON array.
napi_value NativeReaderSpeakers(napi_env env, napi_callback_info /*info*/) {
    return Utf8(env, yoai_reader_speakers());
}

napi_value NativeReaderSetVoice(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    yoai_reader_set_voice(ArgUtf8(env, argc, argv, 0).c_str());
    return nullptr;
}

napi_value NativeReaderSetSpeaker(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    yoai_reader_set_speaker(ArgUtf8(env, argc, argv, 0).c_str());
    return nullptr;
}

void ReaderReferenceWorker(std::string wav, napi_threadsafe_function tsfn) {
    const int rc = yoai_reader_set_reference_voice(wav.c_str());
    AudioEvent* ev = new AudioEvent{rc == 0 ? "end" : "error",
                                    rc == 0 ? "" : yoai_reader_last_error(),
                                    {}, -1};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
}

// nativeReaderSetReferenceVoice(wavPath, onDone: (type, content) => void): void
//
// On a worker, because extracting the embedding runs the model's speaker
// encoder -- about 4.5 s for a 22-second reference -- and on the JS thread
// that froze the window for as long.
napi_value NativeReaderSetReferenceVoice(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_error(env, nullptr, "nativeReaderSetReferenceVoice(wavPath, onDone) needs two arguments");
        return nullptr;
    }
    const std::string path = ArgUtf8(env, argc, argv, 0);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "yoai_reader_reference", NAPI_AUTO_LENGTH, &work_name);
    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[1], nullptr, work_name, 0, 1,
                                        nullptr, nullptr, nullptr, CallJsAudio,
                                        &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }
    Launch(ReaderReferenceWorker, path, tsfn);
    return nullptr;
}

// Returns the empty string on success, or the reason. A file write, so it is
// done here rather than on a worker.
napi_value NativeReaderSaveReferenceEmbedding(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string path = ArgUtf8(env, argc, argv, 0);
    const int rc = yoai_reader_save_reference_embedding(path.c_str());
    return Utf8(env, rc == 0 ? "" : yoai_reader_last_error());
}

// Returns the empty string on success, or the reason. A .spk holds the
// embedding already extracted, so nothing runs and this returns at once.
napi_value NativeReaderLoadReferenceEmbedding(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string path = ArgUtf8(env, argc, argv, 0);
    const int rc = yoai_reader_load_reference_embedding(path.c_str());
    return Utf8(env, rc == 0 ? "" : yoai_reader_last_error());
}

napi_value NativeReaderTalkerHiddenSize(napi_env env, napi_callback_info) {
    napi_value out;
    napi_create_int32(env, yoai_reader_talker_hidden_size(), &out);
    return out;
}

napi_value NativeReaderSetSeed(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double seed = 23.0;
    if (argc >= 1) {
        napi_get_value_double(env, argv[0], &seed);
    }
    yoai_reader_set_seed(static_cast<long long>(seed));
    return nullptr;
}

// nativeReaderSetSplit(minChars, maxChars, tailChars): void. A field <= 0 is
// left at the measured default.
napi_value NativeReaderSetSplit(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t a = 0, b = 0, c = 0;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &a);
    if (argc >= 2) napi_get_value_int32(env, argv[1], &b);
    if (argc >= 3) napi_get_value_int32(env, argv[2], &c);
    yoai_reader_set_split(a, b, c);
    return nullptr;
}

// nativeReaderSetLaterSplit(minChars, maxChars): void. 0 and 0 turn it off.
napi_value NativeReaderSetLaterSplit(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t a = 0, b = 0;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &a);
    if (argc >= 2) napi_get_value_int32(env, argv[1], &b);
    yoai_reader_set_later_split(a, b);
    return nullptr;
}

// nativeReaderSetTemperature(temperature): void. Below 0 restores the default.
napi_value NativeReaderSetTemperature(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double t = -1.0;
    if (argc >= 1) napi_get_value_double(env, argv[0], &t);
    yoai_reader_set_temperature(static_cast<float>(t));
    return nullptr;
}

// nativeReaderSetStreamingText(on): void.
napi_value NativeReaderSetStreamingText(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    bool on = false;
    if (argc >= 1) napi_get_value_bool(env, argv[0], &on);
    yoai_reader_set_streaming_text(on ? 1 : 0);
    return nullptr;
}

// nativeReaderSetWholeReply(on): void.
napi_value NativeReaderSetWholeReply(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    bool on = false;
    if (argc >= 1) napi_get_value_bool(env, argv[0], &on);
    yoai_reader_set_whole_reply(on ? 1 : 0);
    return nullptr;
}

// nativeReaderSetEosHold(frames): void. 0 stops at the first stop code.
napi_value NativeReaderSetEosHold(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t n = 0;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &n);
    yoai_reader_set_eos_hold(n);
    return nullptr;
}

// nativeReaderSetFirstStopChars(chars): void. 0 turns it off.
napi_value NativeReaderSetFirstStopChars(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t n = 0;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &n);
    yoai_reader_set_first_stop_chars(n);
    return nullptr;
}

napi_value NativeReaderSetClauseCuts(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    bool on = false;
    if (argc >= 1) napi_get_value_bool(env, argv[0], &on);
    yoai_reader_set_clause_cuts(on ? 1 : 0);
    return nullptr;
}

napi_value NativeReaderSetThreads(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t n = 0;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &n);
    yoai_reader_set_threads(n);
    return nullptr;
}

// nativeReaderBegin(onAudio: (type, payload, sentence) => void): number
napi_value NativeReaderBegin(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "nativeReaderBegin(onAudio) needs one argument");
        return nullptr;
    }
    if (g_reader_audio != nullptr) {
        return Int32(env, 0);   // a stream is already open
    }

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "yoai_reader", NAPI_AUTO_LENGTH, &work_name);
    if (napi_create_threadsafe_function(env, argv[0], nullptr, work_name, 0, 1,
                                        nullptr, nullptr, nullptr, CallJsAudio,
                                        &g_reader_audio) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }
    if (yoai_reader_begin(OnReaderAudio, g_reader_audio) != 1) {
        napi_release_threadsafe_function(g_reader_audio, napi_tsfn_release);
        g_reader_audio = nullptr;
        return Int32(env, 0);
    }
    return Int32(env, 1);
}

// nativeReaderFeed(delta: string): number. Returns at once.
napi_value NativeReaderFeed(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string delta = ArgUtf8(env, argc, argv, 0);
    return Int32(env, yoai_reader_feed(delta.c_str()));
}

// nativeReaderFinish(onDone: (type, content) => void): void
napi_value NativeReaderFinish(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "nativeReaderFinish(onDone) needs one argument");
        return nullptr;
    }

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "yoai_reader_finish", NAPI_AUTO_LENGTH, &work_name);
    napi_threadsafe_function done = nullptr;
    if (napi_create_threadsafe_function(env, argv[0], nullptr, work_name, 0, 1,
                                        nullptr, nullptr, nullptr, CallJsAudio,
                                        &done) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    napi_threadsafe_function audio = g_reader_audio;
    g_reader_audio = nullptr;
    Launch(ReaderFinishWorker, audio, done);
    return nullptr;
}

// nativeReaderCancel(): void. The stream still has to be finished afterwards.
napi_value NativeReaderCancel(napi_env /*env*/, napi_callback_info /*info*/) {
    yoai_reader_cancel();
    return nullptr;
}

napi_value NativeReaderLastError(napi_env env, napi_callback_info /*info*/) {
    return Utf8(env, yoai_reader_last_error());
}

napi_value NativeTtsLastError(napi_env env, napi_callback_info /*info*/) {
    return Utf8(env, yoai_tts_last_error());
}

//------------------------------------------------------------------------------
// Omni (MNN + Qwen2.5-Omni)
//------------------------------------------------------------------------------
//
// The end-to-end lane: audio in, an answer out, spoken in the model's own
// voice, with no text in the middle. The other two lanes are halves of a chain
// and this one is not, which is the whole reason it is carried -- see
// docs/voice.md for what that costs.
//
// It reuses the audio machinery above because the two agree where it matters:
// chunks of 24 kHz mono float, then a JSON summary. What differs is the INPUT.
// Speech output takes text; this takes 16-bit mono at 16 kHz, so the numbers on
// the two sides of one exchange are not the same rate and a host feeding the
// reply back to a speaker has to say so.

std::atomic<bool> g_omni_busy{false};
// Set by nativeOmniCancel, cleared when an exchange starts. Read once per block
// of speech: returning 0 from the audio handler is how the core is told to
// stop, so the thinker, which hands over no audio, runs to its end regardless.
std::atomic<bool> g_omni_cancel{false};

// Called by the engine on its worker thread, once per block of speech.
int OnOmniAudio(const float* pcm, int n_samples, int /*last*/, void* user_data) {
    if (g_omni_cancel.load(std::memory_order_relaxed)) return 0;
    if (pcm == nullptr || n_samples <= 0) return 1;
    napi_threadsafe_function tsfn = static_cast<napi_threadsafe_function>(user_data);
    AudioEvent* ev = new AudioEvent{"audio", "", std::vector<float>(pcm, pcm + n_samples)};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    return 1;
}

// Called once, on the engine's thread, the moment the thinker stops.
//
// It goes out as its own event instead of waiting for "end" because the two are
// nowhere near each other in time: measured here, the text existed at 3.8 s and
// "end" arrived at 70.3 s. Without this the page showed the answer after the
// speech reading it had finished playing.
void OnOmniText(const char* text, void* user_data) {
    napi_threadsafe_function tsfn = static_cast<napi_threadsafe_function>(user_data);
    AudioEvent* ev = new AudioEvent{"text", text != nullptr ? text : "", {}};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
}

void OmniLoadWorker(std::string dir, napi_threadsafe_function tsfn) {
    const int rc = yoai_omni_load(dir.c_str());
    AudioEvent* ev = new AudioEvent{rc == 0 ? "end" : "error",
                                    rc == 0 ? "" : "the omni model would not load",
                                    {}};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
}

void OmniAskWorker(std::vector<std::int16_t> pcm,
                   std::string prompt,
                   bool want_audio,
                   napi_threadsafe_function tsfn) {
    // The text handler is passed whether or not audio was asked for: it costs
    // one string crossing per exchange, and on a text-only ask it arrives at
    // effectively the same moment as "end" anyway.
    const char* json = yoai_omni_ask(pcm.data(), static_cast<int>(pcm.size()),
                                     prompt.c_str(),
                                     want_audio ? OnOmniAudio : nullptr,
                                     OnOmniText,
                                     tsfn);

    AudioEvent* ev = json != nullptr ? new AudioEvent{"end", json, {}}
                                     : new AudioEvent{"error", "omni could not answer", {}};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
    g_omni_busy.store(false, std::memory_order_relaxed);
}

napi_value NativeOmniAvailable(napi_env env, napi_callback_info /*info*/) {
    return Int32(env, yoai_omni_available());
}

// nativeOmniLoad(modelDir, onDone: (type, content) => void): void
//
// A DIRECTORY, not a file: an MNN model is a config.json plus a dozen .mnn
// graphs, about 3.8 GB together. On a worker for the same reason the other two
// loads are -- reading that much blocks for seconds.
napi_value NativeOmniLoad(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_error(env, nullptr, "nativeOmniLoad(modelDir, onDone) needs two arguments");
        return nullptr;
    }

    std::string dir = ToUtf8(env, argv[0]);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "yoai_omni_load", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[1], nullptr, work_name,
                                        0 /*unbounded*/, 1 /*one worker*/,
                                        nullptr, nullptr, nullptr, CallJsAudio, &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    Launch(OmniLoadWorker, std::move(dir), tsfn);
    return nullptr;
}

napi_value NativeOmniLoaded(napi_env env, napi_callback_info /*info*/) {
    return Int32(env, yoai_omni_loaded());
}

napi_value NativeOmniUnload(napi_env env, napi_callback_info /*info*/) {
    yoai_omni_unload();
    return nullptr;
}

// The one lever on the cost that does not need a different model: MNN runs
// `steps - 1` diffusion forwards per chunk, and what fewer of them coarsens is
// naturalness -- the thing this engine is carried for. See yoai.h.
napi_value NativeOmniSetDitSteps(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t steps = 0;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &steps);
    return Int32(env, yoai_omni_set_dit_steps(steps));
}

// nativeOmniAsk(pcm: Int16Array, prompt, wantAudio, onEvent): number
//
// 1 when the exchange started, 0 when one is already running. The samples are
// COPIED out of the typed array before the worker starts: the caller's buffer
// belongs to the JS heap and may be collected or resized the moment this
// returns, and the engine reads it for as long as the answer takes.
napi_value NativeOmniAsk(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        napi_throw_error(env, nullptr,
                         "nativeOmniAsk(pcm, prompt, wantAudio, onEvent) needs four arguments");
        return nullptr;
    }

    bool expected = false;
    if (!g_omni_busy.compare_exchange_strong(expected, true)) {
        return Int32(env, 0);
    }
    // A cancel meant for the last exchange, or sent when none was running, is
    // not this one's.
    g_omni_cancel.store(false, std::memory_order_relaxed);

    napi_typedarray_type type;
    size_t              length = 0;
    void*               data   = nullptr;
    if (napi_get_typedarray_info(env, argv[0], &type, &length, &data, nullptr, nullptr) != napi_ok ||
        type != napi_int16_array || data == nullptr) {
        g_omni_busy.store(false, std::memory_order_relaxed);
        napi_throw_error(env, nullptr, "nativeOmniAsk: pcm must be an Int16Array");
        return nullptr;
    }
    const std::int16_t* samples = static_cast<const std::int16_t*>(data);
    std::vector<std::int16_t> pcm(samples, samples + length);

    std::string prompt = ToUtf8(env, argv[1]);
    bool want_audio = false;
    napi_get_value_bool(env, argv[2], &want_audio);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "yoai_omni_ask", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[3], nullptr, work_name,
                                        0 /*unbounded*/, 1 /*one worker*/,
                                        nullptr, nullptr, nullptr, CallJsAudio, &tsfn) != napi_ok) {
        g_omni_busy.store(false, std::memory_order_relaxed);
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    Launch(OmniAskWorker, std::move(pcm), std::move(prompt), want_audio, tsfn);
    return Int32(env, 1);
}

// nativeOmniCancel(): void
//
// Stop the running exchange at the Talker's next block of speech. Ignored when
// none is running: the next ask clears it.
napi_value NativeOmniCancel(napi_env /*env*/, napi_callback_info /*info*/) {
    if (g_omni_busy.load(std::memory_order_relaxed)) {
        g_omni_cancel.store(true, std::memory_order_relaxed);
    }
    return nullptr;
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
        {"nativeLocalForget",     nullptr, NativeLocalForget,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeLocalWarm",       nullptr, NativeLocalWarm,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeChat",            nullptr, NativeChat,            nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeChatCancel",      nullptr, NativeChatCancel,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeChatCancelAll",   nullptr, NativeChatCancelAll,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeWorkers",         nullptr, NativeWorkers,         nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativePromptGet",       nullptr, NativePromptGet,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativePromptTurnSystem", nullptr, NativePromptTurnSystem, nullptr, nullptr, nullptr, napi_default, nullptr},

        {"nativeTtsAvailable",    nullptr, NativeTtsAvailable,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderAvailable", nullptr, NativeReaderAvailable, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderLoad", nullptr, NativeReaderLoad, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderLoaded", nullptr, NativeReaderLoaded, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderUnload", nullptr, NativeReaderUnload, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderModelType", nullptr, NativeReaderModelType, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSpeakers", nullptr, NativeReaderSpeakers, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSetVoice", nullptr, NativeReaderSetVoice, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSetSpeaker", nullptr, NativeReaderSetSpeaker, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSetReferenceVoice", nullptr, NativeReaderSetReferenceVoice, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderLoadReferenceEmbedding", nullptr, NativeReaderLoadReferenceEmbedding, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSaveReferenceEmbedding", nullptr, NativeReaderSaveReferenceEmbedding, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderTalkerHiddenSize", nullptr, NativeReaderTalkerHiddenSize, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSetSeed", nullptr, NativeReaderSetSeed, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSetSplit", nullptr, NativeReaderSetSplit, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSetLaterSplit", nullptr, NativeReaderSetLaterSplit, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSetFirstStopChars", nullptr, NativeReaderSetFirstStopChars, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSetTemperature", nullptr, NativeReaderSetTemperature, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSetStreamingText", nullptr, NativeReaderSetStreamingText, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSetEosHold", nullptr, NativeReaderSetEosHold, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSetWholeReply", nullptr, NativeReaderSetWholeReply, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSetClauseCuts", nullptr, NativeReaderSetClauseCuts, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderSetThreads", nullptr, NativeReaderSetThreads, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderBegin", nullptr, NativeReaderBegin, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderFeed", nullptr, NativeReaderFeed, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderFinish", nullptr, NativeReaderFinish, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderCancel", nullptr, NativeReaderCancel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeReaderLastError", nullptr, NativeReaderLastError, nullptr, nullptr, nullptr, napi_default, nullptr},
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

        {"nativeOmniAvailable",   nullptr, NativeOmniAvailable,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeOmniLoad",        nullptr, NativeOmniLoad,        nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeOmniLoaded",      nullptr, NativeOmniLoaded,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeOmniUnload",      nullptr, NativeOmniUnload,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeOmniSetDitSteps", nullptr, NativeOmniSetDitSteps, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeOmniAsk",         nullptr, NativeOmniAsk,         nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeOmniCancel",      nullptr, NativeOmniCancel,      nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
