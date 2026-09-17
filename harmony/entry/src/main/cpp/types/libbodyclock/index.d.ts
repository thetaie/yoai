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
 * Change an on-device model's worker-thread count without unloading it.
 * <= 0 means all cores. Takes effect on the next turn. Returns 0 on success.
 *
 * For measurement: the core logs `on-device threads: <id> -> <n>` here and
 * `on-device turn: ... prefill Nms decode Nms` after each turn, so a sweep is
 * read off the log as pairs of lines.
 */
export const nativeLocalSetThreads: (id: string, threads: number) => number;

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
/**
 * @param grammar GBNF constraining the reply, with `root` as the start rule, or
 *                '' for an ordinary free-text turn. ON-DEVICE MODELS ONLY -- a
 *                remote provider has no equivalent, so the grammar is logged and
 *                ignored there and the reply comes back unconstrained. A grammar
 *                the parser rejects is also logged and ignored rather than
 *                failing the turn.
 */
export const nativeChat: (
  provider: string,
  messagesJson: string,
  systemPrompt: string,
  grammar: string,
  onEvent: (type: string, content: string) => void
) => number;

/**
 * Ask an in-flight turn to stop. It takes effect on the turn's next event and
 * the stream then finishes with 'aborted'; a turn that has gone quiet cannot be
 * interrupted sooner. Unknown or finished ids are a no-op.
 */
export const nativeChatCancel: (turnId: number) => void;

//------------------------------------------------------------------------------
// Voice
//------------------------------------------------------------------------------

/**
 * Whether this build has the recognition engine compiled in.
 *
 * The prosody half does NOT depend on it: nativeVoiceAnalyze still returns its
 * `prosody` section on a build without the engine, and that is the half the
 * emotional reading comes from. Ask this only before offering to add a model.
 */
export const nativeVoiceAvailable: () => boolean;

/**
 * Read a SenseVoice GGUF. Runs off the ArkTS thread -- it is 254 MB at q8 -- and
 * calls back once with ('ok', path) or ('error', reason).
 *
 * The path must be a real filesystem path this process can open, so it comes
 * from the document picker via LocalModelStore, the same as a chat model.
 */
export const nativeVoiceLoad: (
  path: string,
  onDone: (type: string, content: string) => void
) => void;

/** Whether a recognition model is resident right now. */
export const nativeVoiceLoaded: () => boolean;

/** Forget the model. Analysis afterwards returns prosody only. */
export const nativeVoiceUnload: () => number;

/**
 * Inverse text normalization: 1 transcribes a spoken "five hundred and
 * sixty-seven" as '567', 0 as words. Returns 0 on success; needs a loaded model.
 *
 * Not cosmetic. With it on, character counts no longer match syllables, so
 * speaking rate is wrong at every number. With it off the model also returns NO
 * PUNCTUATION at all, which costs the sentence boundaries. Pick per use.
 */
export const nativeVoiceSetItn: (on: number) => number;

/**
 * Analyse one recording: transcript, the model's four tags, per-token
 * timestamps, and the within-recording prosody profile.
 *
 * Runs off the ArkTS thread and calls back once with ('result', json) or
 * ('error', reason). The JSON always carries `audioSeconds`; `transcript` is
 * present only when a model is loaded, and `prosody.enough` is false when there
 * were too few utterances to normalise against -- show nothing then, not a weak
 * number.
 *
 * @param pcm        16-bit signed mono samples, exactly what an AudioCapturer
 *                   hands over. Copied synchronously before the worker starts,
 *                   so the caller may reuse the buffer immediately.
 * @param sampleRate must be 16000. The core has no resampler by design; open the
 *                   capturer at 16 kHz.
 */
export const nativeVoiceAnalyze: (
  pcm: ArrayBuffer,
  sampleRate: number,
  onDone: (type: string, content: string) => void
) => void;

//------------------------------------------------------------------------------
// Omni (MNN)
//------------------------------------------------------------------------------

/**
 * Whether this build has the MNN engine compiled in.
 *
 * This is the second engine, and it is here for exactly one capability: the
 * model answering in its OWN voice. llama.cpp has no Talker in GGUF, so on that
 * lane a reply can only be read aloud by the platform's text-to-speech.
 *
 * Nothing else routes through it. Recognition stays with SenseVoice, the text
 * axes stay with the chat model and its grammar, arousal stays signal
 * processing. See design/voice.md.
 */
export const nativeOmniAvailable: () => boolean;

/**
 * Read an MNN omni model. Runs off the ArkTS thread; calls back once with
 * ('ok', dir) or ('error', reason).
 *
 * `modelDir` is a DIRECTORY, not a file — config.json plus a dozen graphs and
 * their weights, about 3.8 GB. That is the difference from the GGUF lanes: a
 * single-file picker cannot express it.
 */
export const nativeOmniLoad: (
  modelDir: string,
  onDone: (type: string, content: string) => void
) => void;

/** Whether an omni model is resident right now. */
export const nativeOmniLoaded: () => boolean;

/**
 * Ask the model about one recording, optionally aloud.
 *
 * Calls back once with ('result', json) or ('error', reason). The JSON carries
 * `text`, `audioSeconds`, `seconds`, `thinkerSeconds`, `speechSeconds`,
 * `speechElapsed`, `speechRealtime`, `firstAudioSeconds` and `audioBlocks`.
 *
 * `speechRealtime` is seconds of computation per second of speech, and it
 * covers the TALKER ALONE — `seconds` minus `thinkerSeconds` is its numerator.
 * Above 1 the model cannot talk as fast as it speaks.
 *
 * `firstAudioSeconds` is the wait before the first block of PCM, measured from
 * the start of the exchange; `audioBlocks` is how many blocks there were, and 1
 * means nothing streamed. They were added to ask whether playing the reply as
 * it arrives rescues the ratio, and the answer measured here is no: nine blocks
 * carried 10.34 s of speech over 38.5 s, one every 4.3 s holding 1.15 s, so
 * playback drains each in 1.15 s and then waits 3.2 s — 27% speech, 73%
 * silence, which is 1 / 3.7.
 *
 * @param pcm    16-bit signed mono at 16000 Hz, copied synchronously.
 * @param speak  false for a text answer only. true asks the Talker to speak the
 *               reply, which is most of the cost.
 *
 * Audio longer than about 28 seconds is REFUSED, not attempted: MNN segfaults
 * inside its audio front end past roughly 30 s. Chunk before calling.
 */
export const nativeOmniAsk: (
  pcm: ArrayBuffer,
  prompt: string,
  speak: boolean,
  onDone: (type: string, content: string) => void
) => void;

/**
 * The waveform the Talker produced for the last `nativeOmniAsk(..., true, ...)`,
 * as 16-bit signed mono at **24000 Hz** — not the 16 kHz of the input side.
 * Empty when the last call did not ask for speech.
 */
export const nativeOmniLastSpeech: () => ArrayBuffer;

//------------------------------------------------------------------------------
// Model bundles
//------------------------------------------------------------------------------

/**
 * What a bundle contains, without reading its payload. Returns JSON —
 * `{ totalBytes, files: [{ name, bytes }] }` — or '' when the file is not a
 * bundle.
 *
 * Cheap: it reads only the table. Call it before extracting to show what is in
 * there and to check for room.
 */
export const nativeBundleList: (bundlePath: string) => string;

/**
 * Extract a bundle into `destDir`, creating it.
 *
 * One file in, a directory out — which is why bundles exist: an MNN model is
 * about twenty files that MNN resolves relative to config.json, and asking
 * someone to copy twenty files onto a phone and keep the layout is asking them
 * to fail.
 *
 * Runs on a worker (it copies gigabytes) and reports through `onEvent`:
 * ('progress', '0.4213') repeatedly, then exactly one ('ok', destDir) or
 * ('error', reason). Progress is throttled to whole percent.
 *
 * Either it all arrives or nothing does: a failure removes what it wrote,
 * because a truncated weight file is one MNN loads without complaint and then
 * answers through.
 */
export const nativeBundleExtract: (
  bundlePath: string,
  destDir: string,
  onEvent: (type: string, content: string) => void
) => void;
