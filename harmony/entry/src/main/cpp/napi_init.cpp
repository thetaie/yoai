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
#include <map>
#include <memory>
#include <mutex>
#include <string>
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
                napi_threadsafe_function tsfn,
                std::shared_ptr<Turn> turn) {
    HandlerContext ctx{tsfn, turn};

    const int rc = yoai_chat(provider.c_str(), messages_json.c_str(),
                             system_prompt.empty() ? nullptr : system_prompt.c_str(),
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

// nativeChat(provider, messagesJson, systemPrompt,
//            onEvent: (type, content) => void): number   (turn id)
napi_value NativeChat(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        napi_throw_error(env, nullptr,
                         "nativeChat(provider, messagesJson, systemPrompt, onEvent) needs four arguments");
        return nullptr;
    }

    std::string provider = ToUtf8(env, argv[0]);
    std::string messages = ToUtf8(env, argv[1]);
    std::string system   = ToUtf8(env, argv[2]);

    napi_value work_name = nullptr;
    napi_create_string_utf8(env, "yoai_chat", NAPI_AUTO_LENGTH, &work_name);

    napi_threadsafe_function tsfn = nullptr;
    if (napi_create_threadsafe_function(env, argv[3], nullptr, work_name,
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
                std::move(system), tsfn, turn).detach();

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
        {"nativeChat",           nullptr, NativeChat,           nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeChatCancel",     nullptr, NativeChatCancel,     nullptr, nullptr, nullptr, napi_default, nullptr},
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
