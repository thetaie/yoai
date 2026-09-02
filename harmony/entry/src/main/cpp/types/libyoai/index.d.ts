/**
 * NAPI bridge to the embedded YoAI C++ core (the C ABI in src/yoai.h).
 *
 * Call order per session: nativeProviderSet once per model the user has a key
 * for, then nativeChat per turn. Nothing is persisted on the native side -- the
 * registry is process memory, so it has to be refilled after every cold start.
 */

/** Build stamp of the loaded .so. Proves native code is actually running. */
export const nativeVersion: () => string;

/**
 * Core log level: 'debug' | 'info' | 'warn' | 'error'. Output goes to hilog, so
 * it shows up in DevEco's Log window under the tag `yoai`. Default is 'info'.
 */
export const nativeSetLogLevel: (level: string) => void;

/**
 * Register (or replace) one model the user can chat with. Returns 0 on success.
 *
 * @param id        your stable token for this entry, e.g. 'zhipu/glm-5.2'.
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
 * data source.
 */
export const nativeProviders: () => string;

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
