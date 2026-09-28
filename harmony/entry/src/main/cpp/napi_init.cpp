// NAPI bridge: ArkTS <-> the YoAI C ABI (src/yoai.h).
//
// Threading is the whole reason this file exists. yoai_chat BLOCKS until the
// turn completes and calls its handler once per event on the calling thread, so
// each turn runs on a detached worker thread and every event crosses back to
// ArkTS through one napi_threadsafe_function. Events arrive in order; the worker
// appends a synthetic terminal event ("end" on success, "aborted" after a
// cancel) because the C ABI signals completion by returning, not by a sentinel.
//
// Cancellation: nativeChatCancel(turnId) flips a per-turn atomic; the handler
// reads it on the next event and returns 0, which tears down the in-flight HTTP
// transfer inside the core. A turn producing no further events cannot be
// interrupted earlier -- there is nothing to observe the flag.

#include <napi/native_api.h>
#include <hilog/log.h>

#include "yoai.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <utility>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "yoai-napi"

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

// Runs ON THE ArkTS THREAD, once per queued event.
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
    // Blocking, so a slow ArkTS side applies backpressure rather than losing
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
    OH_LOG_INFO(LOG_APP, "chat turn %{public}d finished rc=%{public}d", turn_id, rc);

    // A failing turn has ALREADY sent an "error" event carrying the text worth
    // showing, so the terminal event only has to say how it ended. Cancel is
    // distinguished from failure because the UI treats them differently: one is
    // the user's own doing, the other needs reporting.
    const bool cancelled = turn->cancelled.load(std::memory_order_relaxed);
    StreamEvent* fin = new StreamEvent{cancelled ? "aborted" : "end", ""};
    napi_call_threadsafe_function(tsfn, fin, napi_tsfn_blocking);

    napi_release_threadsafe_function(tsfn, napi_tsfn_release);

    std::lock_guard<std::mutex> lock(g_turns_mutex);
    g_turns.erase(turn_id);
}

//------------------------------------------------------------------------------
// Exported functions
//------------------------------------------------------------------------------

// nativeVersion(): string -- build stamp, proves the .so is really loaded.
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

// nativeLocalAvailable(): boolean -- was this .so built with llama.cpp?
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
// A separate thread for the same reason a turn gets one -- yoai_local_load blocks
// for seconds reading gigabytes, and doing that on the ArkTS thread freezes the
// UI at exactly the moment the user is being told the model is getting ready.
void LoadWorker(std::string id, napi_threadsafe_function tsfn) {
    const int rc = yoai_local_load(id.c_str());
    OH_LOG_INFO(LOG_APP, "local load %{public}s rc=%{public}d", id.c_str(), rc);

    // Reuses the chat callback's (type, content) shape rather than inventing a
    // second one: the ArkTS side already has a handler of that signature, and one
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

// nativeMemoryBandwidth(threads, mib, passes, onDone: (json) => void): void
//
// On a worker, because it blocks: at 256 MiB and four passes it is a gigabyte
// of reads, tens of milliseconds, and a sweep is a dozen of those back to back.
// Run on the UI thread it would drop frames in exactly the way that makes a
// bandwidth number unreliable.
void MemoryBandwidthWorker(int threads, int mib, int passes,
                           napi_threadsafe_function tsfn) {
    const char* json = yoai_memory_bandwidth(threads, mib, passes);
    auto* ev = new StreamEvent{json ? "result" : "error",
                               json ? std::string(json)
                                    : std::string("could not allocate the buffer")};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
}

napi_value NativeMemoryBandwidth(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        napi_throw_error(env, nullptr,
                         "nativeMemoryBandwidth(threads, mib, passes, onDone) needs four arguments");
        return nullptr;
    }
    std::int32_t threads = 0, mib = 0, passes = 0;
    napi_get_value_int32(env, argv[0], &threads);
    napi_get_value_int32(env, argv[1], &mib);
    napi_get_value_int32(env, argv[2], &passes);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "yoai_memory_bandwidth", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[3], nullptr, work_name, 0, 1,
                                        nullptr, nullptr, nullptr, CallJs, &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    std::thread(MemoryBandwidthWorker, threads, mib, passes, tsfn).detach();
    return nullptr;
}

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

    std::thread(LoadWorker, std::move(id), tsfn).detach();
    return nullptr;
}

// nativeChat(provider, messagesJson, systemPrompt,
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
        OH_LOG_INFO(LOG_APP, "chat turn %{public}d cancel requested", id);
    }
    return nullptr;
}

//------------------------------------------------------------------------------
// Voice
//------------------------------------------------------------------------------
//
// Same shape as the on-device model load above: the two calls that block run on
// a detached thread and report through one threadsafe function, because both
// take long enough to drop frames if they ran where the UI draws. Loading is
// 254 MB off disk; analysing a minute of speech is a second or two.

// Runs OFF the ArkTS thread.
void VoiceLoadWorker(std::string path, napi_threadsafe_function tsfn) {
    const int rc = yoai_voice_load(path.c_str());
    auto* ev = new StreamEvent{rc == 0 ? "ok" : "error",
                               rc == 0 ? path : std::string("could not load ") + path};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
}

// Runs OFF the ArkTS thread. Takes the samples BY VALUE: the ArrayBuffer they
// came from belongs to the JS heap, and nothing stops it being collected or
// detached while this runs.
void VoiceAnalyzeWorker(std::vector<std::int16_t> pcm, int sample_rate,
                        napi_threadsafe_function tsfn) {
    const char* json = yoai_voice_analyze(
        pcm.empty() ? nullptr : reinterpret_cast<const short*>(&pcm[0]),
        static_cast<int>(pcm.size()), sample_rate);
    auto* ev = json ? new StreamEvent{"result", std::string(json)}
                    : new StreamEvent{"error", "analysis failed -- see the core log"};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
}

// nativeVoiceAvailable(): boolean
napi_value NativeVoiceAvailable(napi_env env, napi_callback_info) {
    napi_value out = nullptr;
    napi_get_boolean(env, yoai_voice_available() != 0, &out);
    return out;
}

// nativeVoiceLoaded(): boolean
napi_value NativeVoiceLoaded(napi_env env, napi_callback_info) {
    napi_value out = nullptr;
    napi_get_boolean(env, yoai_voice_loaded() != 0, &out);
    return out;
}

// nativeVoiceUnload(): void
napi_value NativeVoiceUnload(napi_env env, napi_callback_info) {
    yoai_voice_unload();
    return Int32(env, 0);
}

// nativeVoiceSetItn(on: number): number
napi_value NativeVoiceSetItn(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t on = 1;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &on);
    return Int32(env, yoai_voice_set_itn(on));
}

// nativeVoiceSetThreads(threads: number): number
//
// Synchronous and cheap -- it sets a field the next transcript reads, and does
// not touch the weights, so a sweep compares thread counts and not page caches.
napi_value NativeVoiceSetThreads(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t n = 0;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &n);
    return Int32(env, yoai_voice_set_threads(n));
}

// nativeVoiceLoad(path, onDone: (type, content) => void): void
napi_value NativeVoiceLoad(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_error(env, nullptr, "nativeVoiceLoad(path, onDone) needs two arguments");
        return nullptr;
    }

    std::string path = ToUtf8(env, argv[0]);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "yoai_voice_load", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[1], nullptr, work_name,
                                        0 /*unbounded*/, 1 /*one worker*/,
                                        nullptr, nullptr, nullptr, CallJs, &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    std::thread(VoiceLoadWorker, std::move(path), tsfn).detach();
    return nullptr;
}

// nativeVoiceAnalyze(pcm: ArrayBuffer, sampleRate: number,
//                    onDone: (type, content) => void): void
//
// `pcm` is 16-bit signed mono, which is what an AudioCapturer hands over, so the
// app never converts and never copies it twice. It must be at `sampleRate`, and
// the core insists on 16000 -- there is no resampler in there on purpose.
napi_value NativeVoiceAnalyze(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 3) {
        napi_throw_error(env, nullptr,
                         "nativeVoiceAnalyze(pcm, sampleRate, onDone) needs three arguments");
        return nullptr;
    }

    void*  data = nullptr;
    size_t bytes = 0;
    if (napi_get_arraybuffer_info(env, argv[0], &data, &bytes) != napi_ok || data == nullptr) {
        napi_throw_error(env, nullptr, "nativeVoiceAnalyze: pcm must be an ArrayBuffer");
        return nullptr;
    }

    std::int32_t sample_rate = 16000;
    napi_get_value_int32(env, argv[1], &sample_rate);

    // Copied here, on the ArkTS thread, while the buffer is certainly alive.
    const size_t count = bytes / sizeof(std::int16_t);
    std::vector<std::int16_t> pcm(count);
    if (count) std::memcpy(&pcm[0], data, count * sizeof(std::int16_t));

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "yoai_voice_analyze", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[2], nullptr, work_name,
                                        0 /*unbounded*/, 1 /*one worker*/,
                                        nullptr, nullptr, nullptr, CallJs, &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    std::thread(VoiceAnalyzeWorker, std::move(pcm), sample_rate, tsfn).detach();
    return nullptr;
}

//------------------------------------------------------------------------------
// Omni (MNN) -- RETIRED
//------------------------------------------------------------------------------
//
// The C entry points these wrap are commented out in src/yoai.h, because
// src/voice/omni.cpp moved to src/voice/attic/ and is in no target. Kept here in
// comment beside the lane that replaced it: the shapes are nearly identical, and
// what separated them was measured rather than argued.
//
// //
// // Same shape as the voice lane: the two calls that block run on a detached
// // thread and report through one threadsafe function.
// //
// // The generated speech is NOT streamed to ArkTS. The C ABI streams it -- that is
// // the general shape -- but a debug page wants the whole waveform to play back,
// // not chunks, so the worker accumulates it here and nativeOmniLastSpeech() hands
// // it over afterwards. One buffer, no per-chunk thread crossings, and the page
// // decides whether it wants the audio at all.
//
// std::mutex g_omni_speech_mutex;
// std::vector<float> g_omni_speech;   // 24 kHz mono, the last reply the Talker spoke
//
// // Runs OFF the ArkTS thread, once per chunk, on the engine's own thread.
// int OmniCollect(const float* pcm, int n_samples, int last, void* user_data) {
//     auto* into = static_cast<std::vector<float>*>(user_data);
//     if (into && pcm && n_samples > 0) {
//         into->insert(into->end(), pcm, pcm + n_samples);
//     }
//     (void)last;
//     return 1;   // never abort: the page asked for the whole reply
// }
//
// void OmniLoadWorker(std::string dir, napi_threadsafe_function tsfn) {
//     const int rc = yoai_omni_load(dir.c_str());
//     auto* ev = new StreamEvent{rc == 0 ? "ok" : "error",
//                                rc == 0 ? dir : std::string("could not load ") + dir};
//     napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
//     napi_release_threadsafe_function(tsfn, napi_tsfn_release);
// }
//
// // Takes the samples BY VALUE -- the ArrayBuffer they came from belongs to the JS
// // heap and nothing stops it being collected while this runs.
// void OmniAskWorker(std::vector<std::int16_t> pcm, std::string prompt, bool speak,
//                    napi_threadsafe_function tsfn) {
//     std::vector<float> speech;
//     const char* json = yoai_omni_ask(
//         pcm.empty() ? nullptr : reinterpret_cast<const short*>(&pcm[0]),
//         static_cast<int>(pcm.size()), prompt.c_str(),
//         speak ? OmniCollect : nullptr,
//         speak ? &speech : nullptr);
//
//     if (speak) {
//         std::lock_guard<std::mutex> lock(g_omni_speech_mutex);
//         g_omni_speech.swap(speech);
//     }
//
//     auto* ev = json ? new StreamEvent{"result", std::string(json)}
//                     : new StreamEvent{"error", "the engine refused or failed -- see the core log"};
//     napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
//     napi_release_threadsafe_function(tsfn, napi_tsfn_release);
// }
//
// // nativeOmniAvailable(): boolean
// napi_value NativeOmniAvailable(napi_env env, napi_callback_info) {
//     napi_value out = nullptr;
//     napi_get_boolean(env, yoai_omni_available() != 0, &out);
//     return out;
// }
//
// // nativeOmniLoaded(): boolean
// napi_value NativeOmniLoaded(napi_env env, napi_callback_info) {
//     napi_value out = nullptr;
//     napi_get_boolean(env, yoai_omni_loaded() != 0, &out);
//     return out;
// }
//
// // nativeOmniSetDitSteps(steps: number): number
// //
// // Synchronous and cheap -- MNN reads the value when it generates, so this sets
// // a field the next ask uses and does not touch the 3.8 GB of weights.
// napi_value NativeOmniSetDitSteps(napi_env env, napi_callback_info info) {
//     size_t argc = 1;
//     napi_value argv[1] = {nullptr};
//     napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
//     std::int32_t n = 0;
//     if (argc >= 1) napi_get_value_int32(env, argv[0], &n);
//     return Int32(env, yoai_omni_set_dit_steps(n));
// }
//
// // nativeOmniLoad(modelDir, onDone: (type, content) => void): void
// napi_value NativeOmniLoad(napi_env env, napi_callback_info info) {
//     size_t argc = 2;
//     napi_value argv[2] = {nullptr, nullptr};
//     napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
//     if (argc < 2) {
        // //         napi_throw_error(env, nullptr, "nativeOmniLoad(modelDir, onDone) needs two arguments");
//         return nullptr;
//     }
//
//     std::string dir = ToUtf8(env, argv[0]);
//
//     napi_value work_name = nullptr;
//     napi_create_string_utf8(env, "yoai_omni_load", NAPI_AUTO_LENGTH, &work_name);
//
//     napi_threadsafe_function tsfn = nullptr;
//     if (napi_create_threadsafe_function(env, argv[1], nullptr, work_name, 0, 1,
//                                         nullptr, nullptr, nullptr, CallJs, &tsfn) != napi_ok) {
//         napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
//         return nullptr;
//     }
//
//     std::thread(OmniLoadWorker, std::move(dir), tsfn).detach();
//     return nullptr;
// }
//
// // nativeOmniAsk(pcm: ArrayBuffer, prompt: string, speak: boolean,
// //               onDone: (type, content) => void): void
// napi_value NativeOmniAsk(napi_env env, napi_callback_info info) {
//     size_t argc = 4;
//     napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
//     napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
//     if (argc < 4) {
//         napi_throw_error(env, nullptr,
        // //                          "nativeOmniAsk(pcm, prompt, speak, onDone) needs four arguments");
//         return nullptr;
//     }
//
//     void*  data  = nullptr;
//     size_t bytes = 0;
//     if (napi_get_arraybuffer_info(env, argv[0], &data, &bytes) != napi_ok || data == nullptr) {
        // //         napi_throw_error(env, nullptr, "nativeOmniAsk: pcm must be an ArrayBuffer");
//         return nullptr;
//     }
//
//     std::string prompt = ToUtf8(env, argv[1]);
//     bool speak = false;
//     napi_get_value_bool(env, argv[2], &speak);
//
//     // Copied here, on the ArkTS thread, while the buffer is certainly alive.
//     const size_t count = bytes / sizeof(std::int16_t);
//     std::vector<std::int16_t> pcm(count);
//     if (count) std::memcpy(&pcm[0], data, count * sizeof(std::int16_t));
//
//     napi_value work_name = nullptr;
//     napi_create_string_utf8(env, "yoai_omni_ask", NAPI_AUTO_LENGTH, &work_name);
//
//     napi_threadsafe_function tsfn = nullptr;
//     if (napi_create_threadsafe_function(env, argv[3], nullptr, work_name, 0, 1,
//                                         nullptr, nullptr, nullptr, CallJs, &tsfn) != napi_ok) {
//         napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
//         return nullptr;
//     }
//
//     std::thread(OmniAskWorker, std::move(pcm), std::move(prompt), speak, tsfn).detach();
//     return nullptr;
// }
//
// // nativeOmniLastSpeech(): ArrayBuffer   (16-bit mono at 24 kHz, empty when none)
// //
// // Converted to 16-bit on the way out rather than handed over as float: it is
// // half the bytes, and it is what an AudioRenderer takes.
// napi_value NativeOmniLastSpeech(napi_env env, napi_callback_info) {
//     std::vector<float> speech;
//     {
//         std::lock_guard<std::mutex> lock(g_omni_speech_mutex);
//         speech = g_omni_speech;
//     }
//
//     void* data = nullptr;
//     napi_value out = nullptr;
//     napi_create_arraybuffer(env, speech.size() * sizeof(std::int16_t), &data, &out);
//     auto* pcm = static_cast<std::int16_t*>(data);
//     for (size_t i = 0; i < speech.size(); ++i) {
//         float v = speech[i] * 32768.0f;
//         if (v > 32767.0f)  v = 32767.0f;
//         if (v < -32768.0f) v = -32768.0f;
//         pcm[i] = static_cast<std::int16_t>(v);
//     }
//     return out;
// }
//
//------------------------------------------------------------------------------
// Speech output (qwentts.cpp)
//------------------------------------------------------------------------------
//
// Same shape as the omni lane above and for the same reasons: the two calls that
// block run on a detached thread, and the waveform is accumulated here rather
// than streamed to ArkTS, because a page wants one buffer to play.
//
// Keeping it separate from the omni lane rather than generalising the two: they
// answer the same question with different models and the point of having both is
// to compare them, which is easier when neither had to bend to fit a shared
// wrapper.

std::mutex g_tts_speech_mutex;
std::vector<float> g_tts_speech;   // 24 kHz mono, the last thing said

// Runs OFF the ArkTS thread, once per chunk, on the engine's compute worker.
int TtsCollect(const float* pcm, int n_samples, int last, void* user_data) {
    auto* into = static_cast<std::vector<float>*>(user_data);
    if (into && pcm && n_samples > 0) {
        into->insert(into->end(), pcm, pcm + n_samples);
    }
    (void)last;   // the end marker arrives empty; there is nothing to do with it
    return 1;     // never abort: the page asked for the whole utterance
}

void TtsLoadWorker(std::string talker, std::string codec, napi_threadsafe_function tsfn) {
    const int rc = yoai_tts_load(talker.c_str(), codec.c_str());
    // The engine's own message on failure, not a rewrite of it: the commonest
    // failure is a talker of the wrong kind, and it says which kind it got.
    auto* ev = new StreamEvent{rc == 0 ? "ok" : "error",
                               rc == 0 ? std::string(yoai_tts_model_type())
                                       : std::string(yoai_tts_last_error())};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
}

void TtsSayWorker(std::string text, bool keep_audio, napi_threadsafe_function tsfn) {
    std::vector<float> speech;
    const char* json = yoai_tts_say(text.c_str(),
                                    keep_audio ? TtsCollect : nullptr,
                                    keep_audio ? &speech : nullptr);
    if (keep_audio) {
        std::lock_guard<std::mutex> lock(g_tts_speech_mutex);
        g_tts_speech.swap(speech);
    }

    auto* ev = json ? new StreamEvent{"result", std::string(json)}
                    : new StreamEvent{"error", std::string(yoai_tts_last_error())};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
}

// nativeTtsAvailable(): boolean
napi_value NativeTtsAvailable(napi_env env, napi_callback_info) {
    napi_value out = nullptr;
    napi_get_boolean(env, yoai_tts_available() != 0, &out);
    return out;
}

// nativeTtsLoaded(): boolean
napi_value NativeTtsLoaded(napi_env env, napi_callback_info) {
    napi_value out = nullptr;
    napi_get_boolean(env, yoai_tts_loaded() != 0, &out);
    return out;
}

// nativeTtsModelType(): string
napi_value NativeTtsModelType(napi_env env, napi_callback_info) {
    return Utf8(env, yoai_tts_model_type());
}

// nativeTtsLastError(): string
napi_value NativeTtsLastError(napi_env env, napi_callback_info) {
    return Utf8(env, yoai_tts_last_error());
}

// nativeTtsLoad(talkerPath, codecPath, onDone: (type, content) => void): void
napi_value NativeTtsLoad(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 3) {
        napi_throw_error(env, nullptr,
                         "nativeTtsLoad(talkerPath, codecPath, onDone) needs three arguments");
        return nullptr;
    }

    std::string talker = ToUtf8(env, argv[0]);
    std::string codec  = ToUtf8(env, argv[1]);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "yoai_tts_load", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[2], nullptr, work_name, 0, 1,
                                        nullptr, nullptr, nullptr, CallJs, &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    std::thread(TtsLoadWorker, std::move(talker), std::move(codec), tsfn).detach();
    return nullptr;
}

// nativeTtsSetVoice(instruct: string): void
//
// Synchronous and cheap: it sets a string the next say() reads, and never
// touches the weights.
napi_value NativeTtsSetVoice(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string instruct = ArgUtf8(env, argc, argv, 0);
    yoai_tts_set_voice(instruct.c_str());
    return nullptr;
}

// nativeTtsSpeakers(): string   (a JSON array, '[]' when the model has none)
napi_value NativeTtsSpeakers(napi_env env, napi_callback_info) {
    return Utf8(env, yoai_tts_speakers());
}

// nativeTtsSetSpeaker(name: string): void
napi_value NativeTtsSetSpeaker(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string name = ArgUtf8(env, argc, argv, 0);
    yoai_tts_set_speaker(name.c_str());
    return nullptr;
}

// nativeTtsSetSeed(seed: number): void
napi_value NativeTtsSetSeed(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int64_t seed = -1;
    if (argc >= 1) napi_get_value_int64(env, argv[0], &seed);
    yoai_tts_set_seed(static_cast<long long>(seed));
    return nullptr;
}

// nativeTtsSetTopK(k: number): void
napi_value NativeTtsSetTopK(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t k = 0;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &k);
    yoai_tts_set_top_k(k);
    return nullptr;
}

// nativeTtsSetReference(wavPath: string, transcript: string): number
//
// Synchronous: it reads a WAV off the disk, which is megabytes rather than
// gigabytes, and the page is picking the file at the time anyway.
napi_value NativeTtsSetReference(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string path = ArgUtf8(env, argc, argv, 0);
    const std::string text = ArgUtf8(env, argc, argv, 1);
    return Int32(env, yoai_tts_set_reference(path.c_str(), text.c_str()));
}

// nativeTtsReferenceSeconds(): number
napi_value NativeTtsReferenceSeconds(napi_env env, napi_callback_info) {
    napi_value out = nullptr;
    napi_create_double(env, yoai_tts_reference_seconds(), &out);
    return out;
}

// nativeTtsSetStreaming(on: boolean): void
napi_value NativeTtsSetStreaming(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    bool on = true;
    if (argc >= 1) napi_get_value_bool(env, argv[0], &on);
    yoai_tts_set_streaming(on ? 1 : 0);
    return nullptr;
}

// nativeTtsSetThreads(n: number): void
//
// Synchronous and cheap, but it only bites on the NEXT load: qwen.h has no
// thread parameter and the count is consumed inside qt_init.
napi_value NativeTtsSetThreads(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::int32_t n = 0;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &n);
    yoai_tts_set_threads(n);
    return nullptr;
}

// nativeTtsSay(text: string, keepAudio: boolean,
//              onDone: (type, content) => void): void
napi_value NativeTtsSay(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 3) {
        napi_throw_error(env, nullptr,
                         "nativeTtsSay(text, keepAudio, onDone) needs three arguments");
        return nullptr;
    }

    std::string text = ToUtf8(env, argv[0]);
    bool keep_audio = false;
    napi_get_value_bool(env, argv[1], &keep_audio);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "yoai_tts_say", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[2], nullptr, work_name, 0, 1,
                                        nullptr, nullptr, nullptr, CallJs, &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    std::thread(TtsSayWorker, std::move(text), keep_audio, tsfn).detach();
    return nullptr;
}

// nativeTtsLastSpeech(): ArrayBuffer   (16-bit mono at 24 kHz, empty when none)
napi_value NativeTtsLastSpeech(napi_env env, napi_callback_info) {
    std::vector<float> speech;
    {
        std::lock_guard<std::mutex> lock(g_tts_speech_mutex);
        speech = g_tts_speech;
    }

    void* data = nullptr;
    napi_value out = nullptr;
    napi_create_arraybuffer(env, speech.size() * sizeof(std::int16_t), &data, &out);
    auto* pcm = static_cast<std::int16_t*>(data);
    for (size_t i = 0; i < speech.size(); ++i) {
        float v = speech[i] * 32768.0f;
        if (v > 32767.0f)  v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm[i] = static_cast<std::int16_t>(v);
    }
    return out;
}

//------------------------------------------------------------------------------
// Model bundles
//------------------------------------------------------------------------------

// Extraction copies several gigabytes, so it runs on a worker and reports
// through the same threadsafe function the other long calls use. Progress
// arrives as ("progress", "0.42") events and then one terminal
// ("ok", dest) or ("error", reason).
//
// Throttled to whole percent. The C ABI offers a callback per megabyte, which
// over 3.7 GB is some 3700 crossings into ArkTS to move a progress bar that has
// a few hundred pixels -- the events would cost more than the copy.
struct BundleJob {
    napi_threadsafe_function tsfn;
    int last_percent;
};

int BundleProgressToJs(const char* name, double fraction, void* user_data) {
    BundleJob* job = static_cast<BundleJob*>(user_data);
    if (!job) {
        return 1;
    }
    const int percent = static_cast<int>(fraction * 100.0);
    if (percent == job->last_percent) {
        return 1;
    }
    job->last_percent = percent;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", fraction);
    auto* ev = new StreamEvent{"progress", std::string(buf)};
    napi_call_threadsafe_function(job->tsfn, ev, napi_tsfn_blocking);
    (void)name;
    return 1;
}

void BundleExtractWorker(std::string bundle, std::string dest,
                         napi_threadsafe_function tsfn) {
    BundleJob job;
    job.tsfn         = tsfn;
    job.last_percent = -1;

    const int rc = yoai_bundle_extract(bundle.c_str(), dest.c_str(),
                                       BundleProgressToJs, &job);

    auto* ev = new StreamEvent{rc == 0 ? "ok" : "error",
                               rc == 0 ? dest
                                       : std::string("could not extract ") + bundle};
    napi_call_threadsafe_function(tsfn, ev, napi_tsfn_blocking);
    napi_release_threadsafe_function(tsfn, napi_tsfn_release);
}

// nativeBundleList(bundlePath): string   ('' when it is not a bundle)
napi_value NativeBundleList(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const std::string path = ArgUtf8(env, argc, argv, 0);
    const char* json = yoai_bundle_list(path.c_str());
    return Utf8(env, json ? json : "");
}

// nativeBundleExtract(bundlePath, destDir, onEvent: (type, content) => void): void
napi_value NativeBundleExtract(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 3) {
        napi_throw_error(env, nullptr,
                         "nativeBundleExtract(bundlePath, destDir, onEvent) needs three arguments");
        return nullptr;
    }

    std::string bundle = ToUtf8(env, argv[0]);
    std::string dest   = ToUtf8(env, argv[1]);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "yoai_bundle_extract", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[2], nullptr, work_name, 0, 1,
                                        nullptr, nullptr, nullptr, CallJs, &tsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "napi_create_threadsafe_function failed");
        return nullptr;
    }

    std::thread(BundleExtractWorker, std::move(bundle), std::move(dest), tsfn).detach();
    return nullptr;
}

}   // namespace

//------------------------------------------------------------------------------
// Module registration
//------------------------------------------------------------------------------

EXTERN_C_START
napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"nativeVersion",        nullptr, NativeVersion,        nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetLogLevel",    nullptr, NativeSetLogLevel,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeProviderSet",    nullptr, NativeProviderSet,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeProviderRemove", nullptr, NativeProviderRemove, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeProviderClear",  nullptr, NativeProviderClear,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeProviders",      nullptr, NativeProviders,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeLocalAvailable", nullptr, NativeLocalAvailable, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeLocalSet",       nullptr, NativeLocalSet,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeLocalLoad",      nullptr, NativeLocalLoad,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeLocalSetThreads", nullptr, NativeLocalSetThreads, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeMemoryBandwidth", nullptr, NativeMemoryBandwidth, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeChat",           nullptr, NativeChat,           nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeChatCancel",     nullptr, NativeChatCancel,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeVoiceAvailable", nullptr, NativeVoiceAvailable, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeVoiceLoad",      nullptr, NativeVoiceLoad,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeVoiceLoaded",    nullptr, NativeVoiceLoaded,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeVoiceUnload",    nullptr, NativeVoiceUnload,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeVoiceSetItn",    nullptr, NativeVoiceSetItn,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeVoiceSetThreads", nullptr, NativeVoiceSetThreads, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeVoiceAnalyze",   nullptr, NativeVoiceAnalyze,   nullptr, nullptr, nullptr, napi_default, nullptr},
        // {"nativeOmniAvailable",  nullptr, NativeOmniAvailable,  nullptr, nullptr, nullptr, napi_default, nullptr},
        // {"nativeOmniLoad",       nullptr, NativeOmniLoad,       nullptr, nullptr, nullptr, napi_default, nullptr},
        // {"nativeOmniLoaded",     nullptr, NativeOmniLoaded,     nullptr, nullptr, nullptr, napi_default, nullptr},
        // {"nativeOmniAsk",        nullptr, NativeOmniAsk,        nullptr, nullptr, nullptr, napi_default, nullptr},
        // {"nativeOmniLastSpeech", nullptr, NativeOmniLastSpeech, nullptr, nullptr, nullptr, napi_default, nullptr},
        // {"nativeOmniSetDitSteps", nullptr, NativeOmniSetDitSteps, nullptr, nullptr, nullptr, napi_default, nullptr},

        {"nativeTtsAvailable",   nullptr, NativeTtsAvailable,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsLoad",        nullptr, NativeTtsLoad,        nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsLoaded",      nullptr, NativeTtsLoaded,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsModelType",   nullptr, NativeTtsModelType,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSetVoice",    nullptr, NativeTtsSetVoice,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSpeakers",    nullptr, NativeTtsSpeakers,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSetSpeaker",  nullptr, NativeTtsSetSpeaker,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSetSeed",     nullptr, NativeTtsSetSeed,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSetTopK",     nullptr, NativeTtsSetTopK,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSetThreads",  nullptr, NativeTtsSetThreads,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSetStreaming", nullptr, NativeTtsSetStreaming, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSetReference", nullptr, NativeTtsSetReference, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsReferenceSeconds", nullptr, NativeTtsReferenceSeconds, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsSay",         nullptr, NativeTtsSay,         nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsLastSpeech",  nullptr, NativeTtsLastSpeech,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeTtsLastError",   nullptr, NativeTtsLastError,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeBundleList",     nullptr, NativeBundleList,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeBundleExtract",  nullptr, NativeBundleExtract,  nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module g_yoai_module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "yoai",   // must match the library name: libyoai.so
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterYoaiModule(void) {
    napi_module_register(&g_yoai_module);
}
