/**
 * NAPI bridge to the embedded YoAI C++ core (the C ABI in src/yoai.h).
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
 * it shows up in DevEco's Log window under the tag `yoai`. Default is 'info'.
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
 * Whether this .so was built with llama.cpp (examples/harmony/build.py llama assembled an
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
 *                    (which are tuned for a phone). See src/yoai.h for the
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
 * How fast this device reads memory, with no language model involved.
 *
 * Calls back once with ('result', json) — `threads`, `bytes`, `passes`,
 * `seconds`, `gbPerSecond` in **decimal** GB — or ('error', reason). Runs on a
 * worker and blocks it for tens of milliseconds per call.
 *
 * `threads` <= 0 means one; sweep it. `mib` <= 0 selects 256, past any phone's
 * last-level cache. `passes` <= 0 selects 4.
 *
 * It exists to check a constant. The thread sweep above showed decode
 * saturating at four threads, which was read as the memory bus filling up — but
 * four is also this SoC's performance-core count, so the curve cannot tell the
 * two apart. A model's throughput is its own kernels as much as the chip's
 * memory; this is a read loop and a division.
 */
export const nativeMemoryBandwidth: (
  threads: number,
  mib: number,
  passes: number,
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
 * Worker threads for the recognition encoder. <= 0 means all cores. Takes effect
 * on the next analysis; the weights are not reloaded, so a sweep compares thread
 * counts rather than page caches. Returns 0 on success; needs a loaded model.
 *
 * The default is 4, carried over from the on-device LLM lane on the reasoning
 * that the pass is memory-bound. It is not: the model is ~250 MB read once per
 * pass, under a percent of the 24.4 GB/s this device was measured to read, and
 * the pass is a GEMM over ~80 stacked frames that reuses every weight 80 times.
 * Compute-bound work is the kind little cores help with, so 4 may be leaving
 * throughput unused — which matters because recognition is the first term of the
 * silence between a person speaking and being answered.
 */
export const nativeVoiceSetThreads: (threads: number) => number;

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
// Omni (MNN) -- RETIRED
//------------------------------------------------------------------------------
//
// None of these are exported any more: src/voice/omni.cpp moved to
// src/voice/attic/ and the NAPI bindings are commented out with it. A
// declaration file that promises a function the module does not export turns a
// build-time error into a runtime one, which is the wrong trade, so these are
// commented rather than left standing.
//
//
// /**
//  * Whether this build has the MNN engine compiled in.
//  *
//  * This is the second engine, and it is here for exactly one capability: the
//  * model answering in its OWN voice. llama.cpp has no Talker in GGUF, so on that
//  * lane a reply can only be read aloud by the platform's text-to-speech.
//  *
//  * Nothing else routes through it. Recognition stays with SenseVoice, the text
//  * axes stay with the chat model and its grammar, arousal stays signal
//  * processing. See docs/voice.md.
//  */
// export const nativeOmniAvailable: () => boolean;
//
// /**
//  * Read an MNN omni model. Runs off the ArkTS thread; calls back once with
//  * ('ok', dir) or ('error', reason).
//  *
//  * `modelDir` is a DIRECTORY, not a file — config.json plus a dozen graphs and
//  * their weights, about 3.8 GB. That is the difference from the GGUF lanes: a
//  * single-file picker cannot express it.
//  */
// export const nativeOmniLoad: (
//   modelDir: string,
//   onDone: (type: string, content: string) => void
// ) => void;
//
// /** Whether an omni model is resident right now. */
// export const nativeOmniLoaded: () => boolean;
//
// /**
//  * Solver steps for the diffusion vocoder. Returns 0 on success; needs a loaded
//  * model, and refuses anything below 2. Takes effect on the next ask without
//  * dropping the weights, so a sweep compares step counts and not page caches.
//  *
//  * The only lever on this lane's 3.7× that does not need a different model. MNN
//  * runs `steps - 1` DiT forwards per chunk, so the shipped 5 is four forwards and
//  * 2 is one. The talker's autoregressive half is affordable on arithmetic — 50
//  * codec tokens a second against 0.392 GB of weights is 19.6 GB/s, inside what
//  * this device reads — so the diffusion is where the excess is.
//  *
//  * It trades the thing being bought: a coarser sampler is a less natural voice,
//  * which is the only reason this engine is a candidate. The number to find is not
//  * the fastest, it is the fewest steps still better than the platform voice.
//  */
// export const nativeOmniSetDitSteps: (steps: number) => number;
//
// /**
//  * Ask the model about one recording, optionally aloud.
//  *
//  * Calls back once with ('result', json) or ('error', reason). The JSON carries
//  * `text`, `audioSeconds`, `seconds`, `thinkerSeconds`, `speechSeconds`,
//  * `speechElapsed`, `speechRealtime`, `firstAudioSeconds` and `audioBlocks`.
//  *
//  * `speechRealtime` is seconds of computation per second of speech, and it
//  * covers the TALKER ALONE — `seconds` minus `thinkerSeconds` is its numerator.
//  * Above 1 the model cannot talk as fast as it speaks.
//  *
//  * `firstAudioSeconds` is the wait before the first block of PCM, measured from
//  * the start of the exchange; `audioBlocks` is how many blocks there were, and 1
//  * means nothing streamed. They were added to ask whether playing the reply as
//  * it arrives rescues the ratio, and the answer measured here is no: nine blocks
//  * carried 10.34 s of speech over 38.5 s, one every 4.3 s holding 1.15 s, so
//  * playback drains each in 1.15 s and then waits 3.2 s — 27% speech, 73%
//  * silence, which is 1 / 3.7.
//  *
//  * @param pcm    16-bit signed mono at 16000 Hz, copied synchronously.
//  * @param speak  false for a text answer only. true asks the Talker to speak the
//  *               reply, which is most of the cost.
//  *
//  * Audio longer than about 28 seconds is REFUSED, not attempted: MNN segfaults
//  * inside its audio front end past roughly 30 s. Chunk before calling.
//  */
// export const nativeOmniAsk: (
//   pcm: ArrayBuffer,
//   prompt: string,
//   speak: boolean,
//   onDone: (type: string, content: string) => void
// ) => void;
//
// /**
//  * The waveform the Talker produced for the last `nativeOmniAsk(..., true, ...)`,
//  * as 16-bit signed mono at **24000 Hz** — not the 16 kHz of the input side.
//  * Empty when the last call did not ask for speech.
//  */
// export const nativeOmniLastSpeech: () => ArrayBuffer;
//
//------------------------------------------------------------------------------
// Speech output (qwentts.cpp + Qwen3-TTS VoiceDesign)
//------------------------------------------------------------------------------
//
// The third engine, and it exists because the second one's answer was no. Omni
// buys a natural voice at about 3× slower than real time with the diffusion
// sampler already at its coarsest, on top of thinking. This one speaks text
// something else produced and measured 1.16× on a desktop.
//
// The voice is WRITTEN, not cloned: `nativeTtsSetVoice` takes an English
// description and the same description gives the same speaker across different
// passages. docs/voice.md records the five other ways of pinning a voice in
// this model family that were tried first, and why each failed.

/** Whether this build has qwentts.cpp compiled in. */
export const nativeTtsAvailable: () => boolean;

/**
 * Read the two GGUFs, about 2.2 GB together. Calls back once with
 * ('ok', modelType) or ('error', reason).
 *
 * `modelType` is 'voice_design', 'base' or 'custom_voice' — the checkpoint
 * decides which parameters are legal, so it is worth showing. The error is the
 * engine's own message: the commonest failure is a talker of the wrong kind,
 * and it says which kind it got.
 */
export const nativeTtsLoad: (
  talkerPath: string,
  codecPath: string,
  onDone: (type: string, content: string) => void
) => void;

/** Whether a TTS model is resident right now. */
export const nativeTtsLoaded: () => boolean;

/** 'voice_design', 'base', 'custom_voice', or '' with nothing loaded. */
export const nativeTtsModelType: () => string;

/**
 * The voice, described in English: `breathy, unhurried but not slow`.
 *
 * A real control surface rather than a preset list — written `gentle, speaks
 * slowly` the result was audibly listless, the words land. Pace, energy,
 * microphone distance and breath are all writable. Takes effect on the next
 * `nativeTtsSay` without dropping the weights.
 */
export const nativeTtsSetVoice: (instruct: string) => void;

/**
 * The named speakers a custom_voice checkpoint carries, as a JSON array of
 * strings — `'[]'` for the other two kinds and before a load.
 *
 * Read from the model rather than hard-coded: "nine named speakers" is
 * something this repo took from a model card and never checked.
 */
export const nativeTtsSpeakers: () => string;

/**
 * Which of them speaks. Takes effect on the next `nativeTtsSay`.
 *
 * **Only legal on a custom_voice checkpoint**, and the core sends it only
 * there, so a name left over from an earlier model cannot break a VoiceDesign
 * run. On that checkpoint it combines with `nativeTtsSetVoice`: the speaker
 * fixes who is talking, the description fixes how.
 */
export const nativeTtsSetSpeaker: (name: string) => void;

/**
 * −1 draws a fresh seed per call.
 *
 * Safe here where on the cloning checkpoint it was a trap: there the seed WAS
 * the speaker, and two seeds gave a man and a woman reading the same sentence.
 * The description pins the identity, so this only varies the delivery.
 */
export const nativeTtsSetSeed: (seed: number) => void;

/** ≤ 0 leaves the engine's own default (50) alone. */
export const nativeTtsSetTopK: (k: number) => void;

/**
 * A recording to sound like, and what is said in it. Returns 0 on success, or
 * non-zero with the reason in `nativeTtsLastError()`.
 *
 * For a `base` checkpoint, which is the cloning one and takes neither a
 * description nor a named speaker. **The WAV must be mono, 16-bit, exactly
 * 24000 Hz** — refused rather than converted, because the core carries no
 * resampler on purpose.
 *
 * The transcript is what makes this IN-CONTEXT cloning rather than a
 * speaker-embedding copy, and that distinction decides the result: pinning a
 * voice through the speaker encoder held the identity and flattened the
 * prosody; carrying the audio and its text in context held both.
 *
 * **It is not free** — the whole reference stays in the talker's context for
 * every frame, so a 22-second clip adds 280 frames of attention to each step.
 *
 * Pass '' for both to drop it.
 */
export const nativeTtsSetReference: (wavPath: string, transcript: string) => number;

/** Seconds of reference currently held; 0 when none. */
export const nativeTtsReferenceSeconds: () => number;

/**
 * The reference voice as a voice file: a .spk, as wide as the loaded talker
 * (2048 values for 1.7B, 1024 for 0.6B). For a `base` checkpoint, after a load.
 * Returns 0 on success, or non-zero with the reason in `nativeTtsLastError()`.
 *
 * What the desktop reads with: one vector, no reference audio, so nothing is
 * added per frame. Exclusive with `nativeTtsSetReference`; setting one drops
 * the other, and `nativeTtsSetReference('', '')` drops both. **A load does not
 * drop it**, so drop it when switching talkers.
 */
export const nativeTtsLoadReferenceEmbedding: (spkPath: string) => number;

/** The talker's sampling temperature. ≤ 0 restores the engine's default (0.9). */
export const nativeTtsSetTemperature: (t: number) => void;

/**
 * The text layout: the talker reads the text one token per frame instead of all
 * of it up front. Off by default.
 */
export const nativeTtsSetStreamingText: (on: boolean) => void;

/** Frames generated past the talker's first stop code. 0, the default, stops at once. */
export const nativeTtsSetEosHold: (frames: number) => void;

/**
 * Whether audio is emitted as it is generated. On by default, and **measured to
 * cost nothing**.
 *
 * It moves the first audio from the end of the run to about a second — 930 to
 * 1109 ms over four runs, against 8.5 to 9.6 s buffered. Seven alternating runs
 * on a pinned seed put buffered's best codec pass at 3073 ms against
 * streaming's 3013, despite streaming re-decoding the codec's left context per
 * chunk.
 *
 * The switch stays because that measurement found something else: the codec
 * stage varies by ±14% on identical input, where the two autoregressive stages
 * hold ±2.5%.
 *
 * **Pin the seed before comparing anything here.** Otherwise the model
 * generates a different number of frames each time and only the per-frame rates
 * survive.
 */
export const nativeTtsSetStreaming: (on: boolean) => void;

/**
 * Worker threads for the NEXT `nativeTtsLoad`. ≤ 0 restores the engine's own
 * choice.
 *
 * **Swept, and it is not a lever**: 4, 6 and 8 land within ~2% of each other,
 * inside the ~9% a five-run sweep drifts from heat, and only 2 is clearly worse
 * (12%). The engine's own 6 costs nothing.
 *
 * **Needs a reload, unlike the llama thread setter** — the count is consumed
 * inside the engine's init and there is no parameter for it. About a second
 * from page cache after the first load.
 */
export const nativeTtsSetThreads: (n: number) => void;

/**
 * Speak `text`. Calls back once with ('result', json) or ('error', reason).
 *
 * The JSON carries `speechSeconds`, `seconds`, `realtime`, `firstAudioSeconds`,
 * `chunks`, `streamed`, `peakDbfs` and `levelDriftDb`. `realtime` is seconds of computation per second of
 * speech and is the number this lane exists to produce; above 1 it cannot talk
 * as fast as it speaks. `streamed` is the path the run actually took, which is
 * not what `nativeTtsSetStreaming` asked for when `keepAudio` is false — with no
 * handler there is nobody to stream to.
 *
 * @param keepAudio false generates and discards, which measures the engine
 *                  without the handover. true keeps the waveform for
 *                  `nativeTtsLastSpeech`.
 */
export const nativeTtsSay: (
  text: string,
  keepAudio: boolean,
  onDone: (type: string, content: string) => void
) => void;

/**
 * The waveform from the last `nativeTtsSay(..., true, ...)`, as 16-bit signed
 * mono at **24000 Hz** — the same rate omni produces, not the 16 kHz of the
 * input side. Empty when the last call did not keep it.
 */
export const nativeTtsLastSpeech: () => ArrayBuffer;

/** Why the last load or say failed. '' after a success. */
export const nativeTtsLastError: () => string;

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
