// Public C ABI for embedding the YoAI core into a host application.
//
// The core is a library, not a server: there is no port to bind, no account,
// and no configuration file. A host app (the HarmonyOS entry module today, an
// Android one next) owns persistence and settings, tells the core which models
// the user has keys for, and asks it to run a turn. That keeps every secret in
// the platform's own secure store and out of any file the core writes.
//
// Deliberately a plain `extern "C"` surface, so any language with a C FFI can
// embed the core -- ArkTS through a NAPI module, Kotlin through JNI, Swift
// through the bridging header, and so on -- without holding a C++ object.
//
// Threading: nothing here starts a thread of its own. yoai_chat BLOCKS for the
// length of the turn, so call it off whatever thread draws. The provider
// registry is guarded internally and safe to edit from another thread while a
// turn is in flight; a turn reads a snapshot taken when it started, so an edit
// never changes a running turn under it.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

//------------------------------------------------------------------------------
// Diagnostics
//------------------------------------------------------------------------------

// The core's version string ("0.1.0"), so a host can log what it linked.
const char* yoai_version(void);

// Process-wide minimum log level, one of "debug", "info", "warn", "error"
// (case-insensitive). An unrecognized value leaves the level unchanged.
// Default is "info", so debug output is silent until this is called.
void yoai_set_log_level(const char* level);

//------------------------------------------------------------------------------
// Provider registry
//------------------------------------------------------------------------------
//
// Which models the user can chat with, and the credentials to reach them. The
// host fills this in at startup from its own settings + secure key store and
// keeps it current as the user adds or removes keys. Nothing here is persisted
// by the core -- the registry lives in process memory only, so a key never
// lands on disk through us.

// Register one model, replacing any entry already under `id`. Returns 0 on
// success, non-zero when a required argument is missing.
//
//   id          the host's stable token for this entry, e.g. "zhipu/glm-5.2".
//               Opaque to the core: whatever you pass here is what yoai_chat
//               takes back and what yoai_providers lists. Required.
//   base_url    OpenAI-compatible API root WITHOUT the /chat/completions
//               suffix, e.g. "https://open.bigmodel.cn/api/paas/v4". Required.
//   api_key     bearer token for that endpoint. Required.
//   model       the model name to send in the request body, e.g. "glm-5.2".
//               Required -- it is the provider's name for the model, which is
//               not always derivable from `id`.
//   extra_json  optional provider-specific request fields, as a JSON OBJECT
//               spliced into the request body's root (e.g.
//               '{"chat_template_kwargs":{"thinking":false}}'). NULL or empty
//               for none. A value that is not a parseable object is logged and
//               ignored, never fatal -- one provider's quirk must not be able
//               to break a turn.
int yoai_provider_set(const char* id,
                      const char* base_url,
                      const char* api_key,
                      const char* model,
                      const char* extra_json);

// Forget one entry. Returns 1 if it was there, 0 if not.
int yoai_provider_remove(const char* id);

// Forget every entry -- what a host calls when the user signs out or clears
// their keys.
void yoai_provider_clear(void);

// The registered ids, NUL-terminated and newline-separated, or an empty string
// when none are registered. This is the menu a host offers the user: an id is
// present exactly when the core has what it needs to run it.
//
// The buffer is owned by the core and stays valid until this thread's next
// yoai_providers() call -- copy it if you need to keep it. Never NULL.
const char* yoai_providers(void);

//------------------------------------------------------------------------------
// Chat
//------------------------------------------------------------------------------

// Streaming callback for yoai_chat, invoked once per event as the turn streams,
// on the thread that called yoai_chat.
//
//   event_type  a stable lowercase tag for the chunk. Never NULL:
//                 "reply"      answer text
//                 "thinking"   reasoning narrative, delivered separately so a
//                              host can collapse it -- a reasoning model's
//                              chain-of-thought is not part of the answer
//                 "usage"      terminal token/cost summary, content is a JSON
//                              object: {"model","inputTokens","outputTokens",
//                              "thoughtTokens","totalTokens","totalCost"}
//                 "error"      provider- or transport-level failure
//               Tool events ("queryTitle" / "queryArguments" / "queryDetail")
//               are reserved: the client can emit them, but nothing registers
//               tools yet, so today they never fire.
//   content     UTF-8 payload, NUL-terminated. Never NULL, may be empty. Valid
//               only for the duration of the call -- copy it to keep it.
//   user_data   the opaque pointer passed to yoai_chat, forwarded verbatim.
//
// Return non-zero to keep streaming, 0 to abort the turn now. Returning 0 is
// how a host cancels: it tears down the HTTP transfer at the next chunk, so no
// separate cancel entry point is needed (and none exists).
typedef int (*yoai_chat_handler)(const char* event_type,
                                 const char* content,
                                 void* user_data);

// Run one chat turn against a registered provider, streaming it through
// `on_event`. BLOCKS until the turn ends or the handler aborts it.
//
//   provider_id     an id from yoai_providers, verbatim. Required; unknown =>
//                   error return.
//   messages_json   '[{"role":"user"|"assistant"|"system","content":"..."},...]'
//                   in conversation order, ending with the current user turn.
//                   Required. Unknown members are ignored; an entry missing
//                   role or content is skipped rather than failing the turn.
//
//                   The WHOLE transcript, every turn: the core holds no session
//                   state, which is what lets the host own history (and decide
//                   what to trim) without two policies disagreeing.
//   system_prompt   may be NULL or empty for none.
//   on_event        streaming callback (see above). Required.
//   user_data       opaque pointer forwarded to every on_event call, may be NULL.
//
// Returns 0 when the turn completed, non-zero on a bad argument, an unknown
// provider, a transport failure, or a handler abort. A provider-level failure
// (401, rate limit, a model that does not exist) is NOT distinguished by the
// return code -- it arrives as an "error" event first, because that is the text
// worth showing the user.
int yoai_chat(const char* provider_id,
              const char* messages_json,
              const char* system_prompt,
              yoai_chat_handler on_event,
              void* user_data);

#ifdef __cplusplus
}
#endif
