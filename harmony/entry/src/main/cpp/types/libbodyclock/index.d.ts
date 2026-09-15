/**
 * NAPI bridge to the embedded Body Clock C++ core (the C ABI in src/bodyclock.h).
 *
 * Call order per session: nativeProviderSet once per model the user has a key
 * for (and nativeLocalSet once per on-device model they have added), then
 * nativeChat per turn. Nothing is persisted on the native side -- the registry is
 * process memory, so it has to be refilled after every cold start.
 */

/** Build stamp of the loaded .so. Proves native code is actually running. */
export const nativeVersion: () => string;

/**
 * Core log level: 'debug' | 'info' | 'warn' | 'error'. Output goes to hilog, so
 * it shows up in DevEco's Log window under the tag `bodyclock`. Default is 'info'.
 */
export const nativeSetLogLevel: (level: string) => void;

/**
 * Register (or replace) one model the user can chat with. Returns 0 on success.
 *
 * @param id        your stable token for this entry, e.g. 'zhipu/glm-4.7-flash'.
 *                  Opaque to the core: it comes back from nativeProviders and
 *                  goes into nativeChat unchanged.
 * @param baseUrl   OpenAI-compatible API root WITHOUT /chat/completions.
 * @param apiKey    bearer token. Held in process memory only -- the core never
 *                  writes it anywhere.
 * @param model     the model name to send in the request body.
 * @param extraJson provider-specific request fields as a JSON object, spliced
 *                  into the request body's root, or '' for none. Malformed
 *                  values are logged and ignored, never fatal.
 */
export const nativeProviderSet: (
  id: string,
  baseUrl: string,
  apiKey: string,
  model: string,
  extraJson: string
) => number;

/** Forget one entry. True if it was there. */
export const nativeProviderRemove: (id: string) => boolean;

/** Forget every entry -- e.g. the user cleared their keys. */
export const nativeProviderClear: () => void;

/**
 * The registered ids, newline-separated, '' when none. An id is listed exactly
 * when the core has everything it needs to run it, so this IS the model picker's
 * data source. Remote and on-device entries are listed together.
 */
export const nativeProviders: () => string;

/**
 * Whether this .so was built with llama.cpp (harmony/build.cmd llama assembled an
 * SDK before the HAP was compiled).
 *
 * False is a supported state, not a failure: the app runs, and everything below
 * still works, but a turn against an on-device model reports the absence as an
 * 'error' event. Check this before offering to add one at all -- letting someone
 * pick a 3 GB file and only then be told is the worse order.
 */
export const nativeLocalAvailable: () => boolean;

/**
 * Register (or replace) one on-device model. Returns 0 on success.
 *
 * Shares the id namespace with nativeProviderSet, so the picker, nativeChat and
 * nativeProviderRemove treat both lanes identically.
 *
 * @param id          your stable token for this entry.
 * @param modelPath   absolute filesystem path to a .gguf. NOT a URI: the engine
 *                    opens and mmaps this path, so a `file://docs/...` string
 *                    fails. Resolve it first (see LocalModelStore), and keep the
 *                    grant alive or the path stops opening after a cold start.
 * @param optionsJson engine knobs as a JSON object, or '' for the defaults
 *                    (which are tuned for a phone). See src/bodyclock.h for the
 *                    members; unknown ones are ignored.
 */
export const nativeLocalSet: (
  id: string,
  modelPath: string,
  optionsJson: string
) => number;

/**
 * Read one on-device model's weights now, on a worker thread, so the first turn
 * does not pay for them. `onDone` fires exactly once: 'end' when the model is
 * resident, 'error' with a reason when it will not load.
 *
 * Worth calling as soon as the user selects a model: the load is seconds and a
 * few GB off disk, and it is the whole difference between a first reply that
 * starts immediately and one that appears to hang.
 *
 * Not cancellable, deliberately -- abandoning a load halfway leaves the file
 * partly read for no benefit.
 */
export const nativeLocalLoad: (
  id: string,
  onDone: (type: string, content: string) => void
) => void;

/**
 * Run one chat turn on a worker thread, streaming events back on the ArkTS
 * thread in arrival order.
 *
 * Event types: 'reply' | 'thinking' | 'usage' | 'error', then exactly one
 * terminal 'end' (finished) or 'aborted' (after nativeChatCancel). 'usage'
 * carries a JSON object; the rest carry text.
 *
 * @param provider     an id from nativeProviders, passed straight through.
 * @param messagesJson the whole conversation as JSON: [{role, content}, ...] in
 *                     order, ending with the current user turn. The core holds
 *                     no session state, so this is the only context it gets.
 * @param systemPrompt system prompt, or '' for none.
 * @returns a turn id for nativeChatCancel, or throws on bad arguments.
 */
export const nativeChat: (
  provider: string,
  messagesJson: string,
  systemPrompt: string,
  onEvent: (type: string, content: string) => void
) => number;

/**
 * Ask an in-flight turn to stop. It takes effect on the turn's next event and
 * the stream then finishes with 'aborted'; a turn that has gone quiet cannot be
 * interrupted sooner. Unknown or finished ids are a no-op.
 */
export const nativeChatCancel: (turnId: number) => void;
