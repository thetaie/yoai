// Public C ABI for embedding the Body Clock core into a host application.
//
// The core is a library, not a server: there is no port to bind, no account,
// and no configuration file. A host app (the HarmonyOS entry module today, an
// Android one next) owns SETTINGS and credentials, tells the core which models
// the user has keys for, and asks it to run a turn. Keys in particular are held
// in process memory only, so they stay in the platform's own secure store and
// out of any file the core writes.
//
// The core does own the day log -- see src/store/timeline.hpp -- because the
// rules about what a day is are the same on every platform and were not going
// to survive being reimplemented per host. Settings belong to the host; the
// record does not.
//
// Deliberately a plain `extern "C"` surface, so any language with a C FFI can
// embed the core -- ArkTS through a NAPI module, Kotlin through JNI, Swift
// through the bridging header, and so on -- without holding a C++ object.
//
// Threading: nothing here starts a thread of its own. bodyclock_chat BLOCKS for the
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
const char* bodyclock_version(void);

// Process-wide minimum log level, one of "debug", "info", "warn", "error"
// (case-insensitive). An unrecognized value leaves the level unchanged.
// Default is "info", so debug output is silent until this is called.
void bodyclock_set_log_level(const char* level);

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
//   id          the host's stable token for this entry, e.g.
//               "zhipu/glm-4.7-flash". Opaque to the core: whatever you pass
//               here is what bodyclock_chat takes back and what bodyclock_providers
//               lists. Required.
//   base_url    OpenAI-compatible API root WITHOUT the /chat/completions
//               suffix, e.g. "https://open.bigmodel.cn/api/paas/v4". Required.
//   api_key     bearer token for that endpoint. Required.
//   model       the model name to send in the request body, e.g.
//               "glm-4.7-flash". Required -- it is the provider's name for the
//               model, which is not always derivable from `id`.
//   extra_json  optional provider-specific request fields, as a JSON OBJECT
//               spliced into the request body's root (e.g.
//               '{"chat_template_kwargs":{"thinking":false}}'). NULL or empty
//               for none. A value that is not a parseable object is logged and
//               ignored, never fatal -- one provider's quirk must not be able
//               to break a turn.
int bodyclock_provider_set(const char* id,
                      const char* base_url,
                      const char* api_key,
                      const char* model,
                      const char* extra_json);

// Forget one entry. Returns 1 if it was there, 0 if not.
int bodyclock_provider_remove(const char* id);

// Forget every entry -- what a host calls when the user signs out or clears
// their keys.
void bodyclock_provider_clear(void);

// The registered ids, NUL-terminated and newline-separated, or an empty string
// when none are registered. This is the menu a host offers the user: an id is
// present exactly when the core has what it needs to run it. Remote and
// on-device entries are listed together -- see the next section.
//
// The buffer is owned by the core and stays valid until this thread's next
// bodyclock_providers() call -- copy it if you need to keep it. Never NULL.
const char* bodyclock_providers(void);

//------------------------------------------------------------------------------
// On-device models (llama.cpp + GGUF)
//------------------------------------------------------------------------------
//
// A second kind of entry in the SAME registry as the remote providers above. An
// on-device id is registered here, then listed by bodyclock_providers, run by
// bodyclock_chat and forgotten by bodyclock_provider_remove exactly like a remote one --
// the host picks a model, not a lane. Which one an id is stays the core's
// business, so a host that never adds a local model is unaffected by all of
// this.
//
// The weights are read from a path and mmapped, so `model_path` must be a real
// filesystem path the process can open -- not a URI, not a file descriptor. On
// HarmonyOS a file the user grants through the document picker satisfies that;
// see harmony's LocalModelStore for the grant handling that keeps it true across
// launches.

// Whether this build has the engine compiled in: 1 if yes, 0 if it was built
// without llama.cpp. When 0, everything below still exists and answers
// truthfully -- bodyclock_local_set succeeds and the entry appears in the menu, but a
// turn against it reports the absence as an "error" event. A host should call
// this before offering an "add an on-device model" affordance at all.
int bodyclock_local_available(void);

// Register one on-device model, replacing any entry already under `id`. Returns
// 0 on success, non-zero when a required argument is missing.
//
//   id            the host's stable token for this entry, as with
//                 bodyclock_provider_set. Shares one namespace with the remote
//                 entries, so registering a local model under an id already
//                 held by a remote one replaces it. Required.
//   model_path    absolute path to a .gguf the process can open. Required. NOT
//                 validated here -- the file is opened on the first turn (or by
//                 bodyclock_local_load), so a path that has gone stale reports itself
//                 as an "error" event rather than at registration time.
//   options_json  optional JSON OBJECT of engine knobs; NULL or empty for the
//                 defaults, which are tuned for a phone. Unknown members are
//                 ignored, and a value that is not a parseable object is logged
//                 and ignored rather than failing. Members, all optional:
//                   "threads"        int   worker threads; <= 0 means all cores.
//                                          Default 4 -- on a big.LITTLE phone
//                                          the all-cores default is routinely
//                                          the slowest, because the batch waits
//                                          on the little cores. Measured on a
//                                          Kirin 9020 (1+3+4), both prefill and
//                                          decode stop improving at four, which
//                                          is its performance-core count; see
//                                          llm/local.hpp for the table.
//                   "contextMax"     int   ceiling for the context window; the
//                                          turn takes the largest size that both
//                                          the model supports and the device can
//                                          allocate. Default 32768.
//                   "predict"        int   ceiling on tokens per reply; <= 0
//                                          means only the context limits it.
//                                          Default 8192, a runaway backstop
//                                          rather than a policy cap.
//                   "replyReserve"   int   tokens kept free for the answer;
//                                          history is trimmed to fit around it.
//                                          Default 1024.
//                   "thinking"       int   1 on, 0 off, -1 leave it to the
//                                          model. Default 0.
//                   "temperature"    float default 0.7; <= 0 selects greedy
//                                          decoding, which loops -- for
//                                          benchmarks, not for people.
//                   "topP"           float default 0.8
//                   "topK"           int   default 20
//                   "repeatPenalty"  float default 1.1; 1.0 disables
//                   "repeatLastN"    int   default 256
int bodyclock_local_set(const char* id,
                   const char* model_path,
                   const char* options_json);

// Read one registered on-device model's weights now, so the first turn does not
// pay for them. BLOCKS for the length of the load (seconds, a few GB off disk),
// so call it off the drawing thread -- ideally while the user is still typing.
//
// Returns 0 when the model is resident, non-zero for an unknown id, an id that
// is a remote provider, a build without the engine, or a file that will not
// load. Idempotent: a second call on a loaded model returns 0 immediately.
int bodyclock_local_load(const char* id);

// Change the worker-thread count of an already-registered on-device model,
// WITHOUT dropping the loaded weights. <= 0 means all cores.
//
// bodyclock_local_set would do this too, but it replaces the registry entry and
// therefore discards the cached engine -- re-reading a gigabyte or two off
// storage between one setting and the next, which makes a sweep both slow and
// unreliable: what is being compared then includes a cold page cache.
//
// Takes effect on the NEXT turn. Returns 0, or non-zero if `id` is unknown or
// is not an on-device model.
//
// This exists for a specific measurement, and that measurement has been taken.
// The claim it was added to settle -- that one core already saturates the
// memory controller -- is false: decode rises 5.8 -> 12.6 tok/s from one thread
// to four (see llm/local.hpp for the sweep), so one core reaches well under
// half of what the chip gives up.
//
// **The other half of the question it was framed around was a false choice.**
// "Flat means a per-chip wall, rising means per-core" leaves out the reading
// that turned out to be right: throughput plateaus at four because four is this
// SoC's performance-core count, which says nothing about the bus at all. No
// thread sweep of a language model can separate those -- every point on it is
// one engine's kernels as much as the hardware. That is what
// bodyclock_memory_bandwidth below is for.
int bodyclock_local_set_threads(const char* id, int threads);

// How fast this machine reads memory, measured WITHOUT a language model.
//
// Returns a JSON object -- threads, bytes, passes, seconds, gbPerSecond (in
// decimal GB, the unit the figures in design/voice.md use) -- owned by the core
// and valid until this thread's next call. NULL if the buffer would not
// allocate. BLOCKS for tens of milliseconds per call.
//
//   threads  <= 0 means one. Sweep it; nothing here guesses a good value.
//   mib      <= 0 selects 256, past any phone's last-level cache.
//   passes   <= 0 selects 4.
//
// This exists for the same argument bodyclock_local_set_threads was added for,
// one step further down. That sweep showed decode saturating at four threads and
// was read as the memory bus filling up -- but it saturates at the number of
// performance cores, which is equally consistent with running out of cores worth
// using. A language model cannot separate those: every number it gives is one
// engine's kernels as much as the chip. This reads a large buffer and divides.
const char* bodyclock_memory_bandwidth(int threads, int mib, int passes);

//------------------------------------------------------------------------------
// Chat
//------------------------------------------------------------------------------

// Streaming callback for bodyclock_chat, invoked once per event as the turn streams,
// on the thread that called bodyclock_chat.
//
//   event_type  a stable lowercase tag for the chunk. Never NULL:
//                 "reply"      answer text
//                 "thinking"   reasoning narrative, delivered separately so a
//                              host can collapse it -- a reasoning model's
//                              chain-of-thought is not part of the answer
//                 "usage"      terminal token/cost summary, content is a JSON
//                              object: {"model","inputTokens","outputTokens",
//                              "thoughtTokens","totalTokens","totalCost"}.
//                              An on-device turn reports it too, counting the
//                              same way with a cost of 0 -- the token figures
//                              are what the model actually processed, and are
//                              worth showing whether or not anyone was billed.
//                 "error"      provider- or transport-level failure
//               Tool events ("queryTitle" / "queryArguments" / "queryDetail")
//               are reserved: the client can emit them, but nothing registers
//               tools yet, so today they never fire.
//   content     UTF-8 payload, NUL-terminated. Never NULL, may be empty. Valid
//               only for the duration of the call -- copy it to keep it.
//   user_data   the opaque pointer passed to bodyclock_chat, forwarded verbatim.
//
// Return non-zero to keep streaming, 0 to abort the turn now. Returning 0 is
// how a host cancels: it tears down the HTTP transfer at the next chunk -- or,
// on an on-device turn, stops the decode loop at the next token -- so no
// separate cancel entry point is needed (and none exists).
typedef int (*bodyclock_chat_handler)(const char* event_type,
                                 const char* content,
                                 void* user_data);

// Run one chat turn against a registered provider, streaming it through
// `on_event`. BLOCKS until the turn ends or the handler aborts it.
//
//   provider_id     an id from bodyclock_providers, verbatim. Required; unknown =>
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
//   grammar         optional GBNF constraining the reply, with `root` as the
//                   start rule. NULL or empty for an ordinary free-text turn,
//                   which is every conversational one.
//
//                   ON-DEVICE ONLY. A remote provider cannot be given a GBNF --
//                   it is a llama.cpp concept with no equivalent in the OpenAI
//                   protocol -- so the grammar is logged and ignored there, and
//                   the reply comes back unconstrained. A caller that must have
//                   structured output from a remote model wants that provider's
//                   own `response_format`, through bodyclock_provider_set's
//                   `extra_json`.
//
//                   A grammar the parser rejects is logged and the turn runs
//                   unconstrained, rather than failing: a visibly wrong answer
//                   with a reason in the log beats a turn that never ran.
//
//                   PASSING A GRAMMAR ALSO MAKES THE TURN DETERMINISTIC on the
//                   on-device lane -- greedy, no repetition penalty, whatever
//                   the model's chat settings are. A grammar means the reply is
//                   a value rather than prose, and a value that changes between
//                   runs on one input is not a value. Measured: the same 2B
//                   scoring one sentence answered -1, -1, -1, 0, -1, 0 at the
//                   conversational defaults, and 0 every time greedily.
//   on_event        streaming callback (see above). Required.
//   user_data       opaque pointer forwarded to every on_event call, may be NULL.
//
// Returns 0 when the turn completed, non-zero on a bad argument, an unknown
// provider, a transport failure, or a handler abort. A provider-level failure
// (401, rate limit, a model that does not exist) is NOT distinguished by the
// return code -- it arrives as an "error" event first, because that is the text
// worth showing the user.
int bodyclock_chat(const char* provider_id,
              const char* messages_json,
              const char* system_prompt,
              const char* grammar,
              bodyclock_chat_handler on_event,
              void* user_data);

//------------------------------------------------------------------------------
// Voice
//------------------------------------------------------------------------------
//
// One recording in, everything the core can say about it out. Two independent
// halves, and it is worth knowing which is which:
//
//   - Recognition (SenseVoice) needs a model file and the engine compiled in.
//   - The prosody measurements need neither. They are signal processing, they
//     run on any build, and they are what "how did this person sound" is read
//     from -- not from the model's own emotion label, which has no word for
//     tired, depleted or calm. See design/voice.md.
//
// So a host that never loads a model still gets the half that matters most.

// Whether this build has the recognition engine compiled in: 1 if yes, 0 if it
// was built without it. When 0, bodyclock_voice_load fails and says so, and
// analysis still returns its prosody half. Ask before offering to add a model.
int bodyclock_voice_available(void);

// Read a SenseVoice GGUF. BLOCKS for the length of the load (254 MB at q8), so
// call it off the drawing thread. Returns 0 when the model is resident.
//
// As with the on-device LLM, `model_path` must be a real filesystem path this
// process can open -- on HarmonyOS, a file the user granted through the document
// picker. A second load replaces the first.
int bodyclock_voice_load(const char* model_path);

// Whether a model is resident right now.
int bodyclock_voice_loaded(void);

// Drop the weights. Analysis afterwards returns prosody only.
void bodyclock_voice_unload(void);

// Inverse text normalization: with it on, a spoken "five hundred and sixty-seven"
// is transcribed "567". The model ships with it on. Pass 0 to turn it off, which
// is what anything counting syllables wants -- and be aware that the same switch
// also removes ALL punctuation, so it is not free. Returns 0 on success.
//
// Requires a loaded model; it changes what the model is asked, not how the
// answer is read.
int bodyclock_voice_set_itn(int on);

// Worker threads for the encoder pass. <= 0 means all cores. Takes effect on the
// next analysis. Returns 0 on success.
//
// Requires a loaded model, and does NOT reload it -- so a sweep compares thread
// counts rather than page caches, the same reason bodyclock_local_set_threads
// exists.
//
// **The default is 4 and the reason given for it does not hold.** The header it
// came from says the encoder pass is "memory-bound enough that a phone's little
// cores hold the batch up", and the arithmetic says otherwise: the model is
// about 250 MB, read once per pass, which even at a slow pass is under a percent
// of the 24.4 GB/s this device was measured to read. It is a GEMM over roughly
// eighty stacked frames, reusing every weight eighty times -- compute-bound, and
// compute-bound work is what little cores can help with.
//
// This exists so the default can be chosen by measurement instead. Recognition
// is the first term of the silence a person sits through between speaking and
// being answered, and it is the term nobody has timed on a phone.
int bodyclock_voice_set_threads(int threads);

// Analyse one recording. Returns a JSON object, or NULL on a bad argument.
//
//   pcm           16-bit signed mono samples.
//   n_samples     how many. A clip under one analysis window returns an object
//                 whose halves are both absent rather than an error.
//   sample_rate   must be 16000. The core carries no resampler on purpose: a
//                 resampler that differs from the one a measurement was taken
//                 with changes the answer quietly, which this project has
//                 already been bitten by. Capture at 16 kHz.
//
// The object always carries `audioSeconds`. `transcript` is present only when a
// model is loaded, `prosody` only when the clip was long enough to frame, and
// `prosody.enough` is false when there were too few utterances to normalize
// against -- fewer than about five makes the deviations noise, and a host should
// show nothing rather than a weak number.
//
// Each utterance carries the transcript tokens whose timestamps fall inside it,
// so the profile can be read against what was being said. That alignment is
// 60 ms granular and lands on the frame where the token was EMITTED, which lags
// slightly -- good enough to read a table by, not a subtitle track.
//
// The buffer is owned by the core and stays valid until this thread's next
// bodyclock_voice_analyze() call. Copy it to keep it.
const char* bodyclock_voice_analyze(const short* pcm,
                               int n_samples,
                               int sample_rate);

//------------------------------------------------------------------------------
// Omni (MNN) -- RETIRED
//------------------------------------------------------------------------------
//
// Declarations for entry points that no longer exist: src/voice/omni.cpp moved
// to src/voice/attic/ and is in no target. A header that promises a symbol the
// library does not define is worse than a missing one -- it fails at link, in
// somebody else's build -- so these are commented rather than left standing.
//
// The lane below replaced it and reads as a narrower version of the same shape.
// src/voice/attic/README.md has the measurements that decided it.
//
// //
// // A SECOND engine, for the one thing the first cannot do: speak in the model's
// // own voice. llama.cpp has no Talker checkpoint in GGUF; MNN has the whole chain
// // -- thinker, talker, diffusion transformer, vocoder -- and it works. Measured
// // on a desktop: audio in, a spoken answer out, in one call.
// //
// // It is NOT a replacement for anything. Recognition stays with SenseVoice, which
// // is cheap and uniquely supplies the 60 ms alignment the clause cut needs; text
// // stays with llama.cpp and its grammar; prosody is signal processing and needs no
// // engine at all. This lane buys exactly one thing, and design/voice.md argues
// // over whether that one thing is affordable. These entry points exist so the
// // question can be answered on a phone instead of a workstation.
// //
// // Threading and cost: everything here BLOCKS, and generating speech measured
// // about 3x slower than real time on a desktop CPU -- a fifteen-second reply is
// // most of a minute. Call it off the drawing thread and expect to wait.
//
// // Whether this build has the engine compiled in: 1 if yes, 0 if it was built
// // without MNN. When 0 everything below still links and answers truthfully.
// int bodyclock_omni_available(void);
//
// // Read a model. BLOCKS.
// //
// // `model_dir` is a DIRECTORY, not a file -- and that is the difference from the
// // other two lanes. An MNN model is config.json plus a dozen .mnn graphs and
// // their weights (thinker, audio encoder, talker, predit, dit, bigvgan, speaker
// // dictionary), about 3.8 GB together. A host whose file picker hands back single
// // files has to pick the directory instead.
// //
// // Returns 0 when the model is resident.
// int bodyclock_omni_load(const char* model_dir);
//
// int bodyclock_omni_loaded(void);
//
// // Drop the weights.
// void bodyclock_omni_unload(void);
//
// // How many solver steps the diffusion vocoder takes. Returns 0 on success, or
// // non-zero with no model loaded or for a count below 2.
// //
// // **This is the one lever on the 3.7x that does not need a different model.**
// // The chain that makes sound is an autoregressive talker, then a diffusion
// // transformer, then a vocoder. The talker's half is affordable on arithmetic:
// // codec tokens run at 50 Hz and its weights are 0.392 GB, so one second of
// // audio needs 19.6 GB/s of reads -- inside what this device was measured to do.
// // The excess is the diffusion, and MNN runs `steps - 1` DiT forwards per chunk.
// // The shipped config says 5, which is four forwards; 2 is one.
// //
// // Read at GENERATION time, not load time, so this takes effect on the next ask
// // and the model stays resident -- the same reason bodyclock_local_set_threads
// // exists, and it makes a sweep compare step counts rather than page caches.
// //
// // It trades the thing being bought. Fewer steps is a coarser sampler, and what
// // it coarsens is naturalness, which is the whole reason this engine is being
// // considered. So the number to find is not the fastest one: it is the fewest
// // steps that still sound better than the platform voice.
// int bodyclock_omni_set_dit_steps(int steps);
//
// // Streaming callback for the generated voice, invoked as the waveform is
// // produced. `pcm` is mono float at 24000 Hz -- note that this is NOT the 16 kHz
// // the input side uses, so a host feeding it back to a speaker must say so.
// //
// // Return non-zero to keep going, 0 to abort. `last` is 1 on the final chunk.
// typedef int (*bodyclock_omni_audio_handler)(const float* pcm,
//                                        int n_samples,
//                                        int last,
//                                        void* user_data);
//
// // Ask the model about one recording, and optionally have it answer aloud.
// //
// //   pcm, n_samples   16-bit signed mono at 16000 Hz, as with the voice lane.
// //   prompt           what to ask about the audio. Required.
// //   on_audio         NULL for a text-only answer. Non-NULL asks the Talker to
// //                    speak the reply, which is most of the cost.
// //
// // Returns a JSON object, owned by the core and valid until this thread's next
// // call, or NULL on a bad argument or an engine failure. It carries the reply
// // `text` and the timings the decision in design/voice.md turns on:
// //
// //   speechRealtime      seconds of computation per second of speech, for the
// //                       TALKER alone. Above 1 it cannot talk as fast as it
// //                       speaks. Measured 3.7 on a phone.
// //   firstAudioSeconds   the wait before the first PCM arrives, from the start
// //                       of the exchange.
// //   audioBlocks         how many handovers that took. 1 means nothing streamed.
// //
// // Those two were added to ask whether playing the reply as it arrives rescues
// // the ratio. Measured: no. Nine blocks carried 10.34 s of speech over 38.5 s --
// // one every 4.3 s holding 1.15 s of audio, so playback drains each in 1.15 s
// // and then waits 3.2 s. 27% speech, 73% silence, which is 1 / 3.7. Streaming
// // moves the first word much earlier and cannot close the gaps.
// //   thinkerSeconds      audio in to last text token; `seconds` is everything.
// //
// // **Audio over about 30 seconds is refused rather than attempted**, and the
// // number is not a limit. It was set from two runs that read as a boundary;
// // re-measured, the same clip at the same length both crashes and succeeds, at
// // any thread count. The guard shortens the input rather than avoiding the bug.
// // See src/voice/attic/omni.hpp and design/voice.md.
// const char* bodyclock_omni_ask(const short* pcm,
//                           int n_samples,
//                           const char* prompt,
//                           bodyclock_omni_audio_handler on_audio,
//                           void* user_data);
//
//------------------------------------------------------------------------------
// Speech output (qwentts.cpp + Qwen3-TTS VoiceDesign)
//------------------------------------------------------------------------------
//
// The THIRD engine, and it exists because the second one's answer was no. The
// omni lane above buys a natural voice at about 3x slower than real time even
// with the diffusion sampler at its coarsest, and that cost is paid on top of
// thinking. This lane speaks text something else produced -- llama.cpp's, at
// 14 tok/s -- and measured 1.16x on a desktop for the same sentences.
//
// **The voice is written, not cloned.** bodyclock_tts_set_voice takes an English
// description and the same description yields the same speaker across different
// passages. Five other ways of getting a fixed voice out of this model family
// were tried first and design/voice.md records why each failed; the short form
// is that the cloning checkpoint either changed speaker between texts or held
// the speaker and flattened the prosody.
//
// Two GGUF files: the 1.7B VoiceDesign pair is 1.4 GB at Q4_K_M, the 0.6B
// CustomVoice pair 0.8 GB and about a tenth faster. CustomVoice keeps a written
// delivery -- see bodyclock_tts_set_speaker.
//
// **On the phone it has TWO speeds: 2.14x for the first twenty to thirty
// seconds of continuous generation, then 3.52x (1.94x and 3.30x on the 0.6B).**
// The step is a clock step, it comes sooner when the device is already warm, and
// model size does not move it. Anything measured on a short utterance is a
// sprint figure; a 30-second passage takes 60 to 97 seconds, and that is the
// case this lane is actually for.
//
// Of the fast regime, 1.45x is there with a talker costing nothing: the codec
// alone is 0.92x -- 73.5 ms per 80 ms frame, unmoved by buffering instead of
// streaming, by its own quantization, or by a talker a third the size -- and the
// code predictor is sized by the codebook count rather than by the LM. The
// talker does respond to its own quantization (22% from Q8 to Q4); the thread
// count responds to nothing. So this lane is for PREPARED PLAYBACK: generate
// while nobody is waiting, play later. Conversation has its answer already, and
// it is the platform voice.
//
// Threading and cost: everything here BLOCKS.

// Whether this build has the engine compiled in: 1 if yes, 0 if it was built
// without qwentts.cpp. When 0 everything below still links and answers
// truthfully.
int bodyclock_tts_available(void);

// Read the two models. BLOCKS.
//
// `talker_path` is the LM and code predictor, `codec_path` the 12 Hz audio
// tokenizer. Two files rather than a directory, which is the difference from the
// omni lane -- these are GGUF and self-describing, so nothing has to resolve
// siblings relative to a config.
//
// Returns 0 when both are resident. On failure the reason is in
// bodyclock_tts_last_error(), and it is worth showing: the commonest one is a
// talker of the wrong kind, which is a file name mistake and not a broken model.
int bodyclock_tts_load(const char* talker_path, const char* codec_path);

int bodyclock_tts_loaded(void);

// Drop the weights.
void bodyclock_tts_unload(void);

// What the loaded talker is: "voice_design", "base" or "custom_voice", or ""
// with nothing loaded. Owned by the core.
//
// Worth surfacing because the three take DIFFERENT parameters -- a voice
// description is required by one and refused by another -- so a host that lets
// someone pick a file has to be able to say which one they picked.
const char* bodyclock_tts_model_type(void);

// The named speakers a custom_voice checkpoint carries, as a JSON array of
// strings -- `[]` for the other two kinds and before a load. Owned by the core
// and valid until this thread's next call.
//
// Read from the model rather than hard-coded. "Nine named speakers" is
// something this repo took from a model card and never checked, and a list that
// came out of the file cannot be stale.
const char* bodyclock_tts_speakers(void);

// Which of them speaks. Takes effect on the next say().
//
// **Only legal on a custom_voice checkpoint**, and the core sends it only
// there, so a name left over from an earlier model cannot break a VoiceDesign
// run. "" asks the engine for its own default, which on this checkpoint is a
// speaker rather than silence.
void bodyclock_tts_set_speaker(const char* name);

// The voice, described in English: "breathy, unhurried but not slow".
//
// A real control surface rather than a preset list. Written `gentle, speaks
// slowly` the result was audibly listless -- the words land. Pace, energy,
// microphone distance and breath are all writable.
//
// Required by voice_design, OPTIONAL on custom_voice -- where it combines with
// the speaker, who fixes identity while this describes delivery -- and refused
// outright by base.
//
// Takes effect on the next say(); the weights stay resident, so a sweep compares
// descriptions rather than page caches.
void bodyclock_tts_set_voice(const char* instruct);

// Sampling. -1 draws a fresh seed per call.
//
// Safe to expose here where on the cloning checkpoint it would have been a trap:
// there the seed WAS the speaker, and two seeds gave a man and a woman reading
// the same sentence. The description pins the identity, so this only varies the
// delivery.
void bodyclock_tts_set_seed(long long seed);

// <= 0 leaves the engine's own default (50) alone.
void bodyclock_tts_set_top_k(int top_k);

// A recording to sound like, and what is said in it. For a `base` checkpoint,
// which is the cloning one and takes neither a description nor a named speaker.
//
// Returns 0 when the reference is held. **The WAV must be mono, 16-bit, exactly
// 24000 Hz**, and one that is not is refused rather than converted -- the same
// rule bodyclock_voice_analyze states at 16 kHz, for the same reason. On failure
// the reason is in bodyclock_tts_last_error().
//
// The transcript is what makes this IN-CONTEXT cloning rather than a
// speaker-embedding copy, and design/voice.md records why that distinction
// decides the result: pinning a voice through the speaker encoder held the
// identity and flattened the prosody; carrying the audio and its text in
// context held both.
//
// **It is not free.** The whole reference stays in the talker's context for
// every frame, so a 22-second clip adds 280 frames of attention to each step.
// On the desktop that measured 70% dearer than a described voice. A shorter
// reference is cheaper and has not been tried.
//
// Pass NULL or "" for both to drop it.
int bodyclock_tts_set_reference(const char* wav_path, const char* transcript);

// Seconds of reference currently held; 0 when none.
float bodyclock_tts_reference_seconds(void);

// Whether audio is emitted as it is generated. Non-zero (the default) streams
// it; 0 decodes the whole utterance in one pass and hands it over once.
//
// **Measured, and streaming costs nothing.** It moves the first audio from the
// end of the run to about a second, and seven alternating runs on a pinned seed
// put buffered's best codec pass at 3073 ms against streaming's 3013 -- no
// faster, despite streaming re-decoding the codec's left context per chunk.
//
// The switch stays because that measurement found something else: the codec
// stage varies by +-14% on identical input, where the two autoregressive stages
// hold +-2.5%. Any future claim about that stage has to hold this fixed first.
//
// **Pin the seed before comparing anything here**, or the model generates a
// different number of frames each time and only the per-frame rates survive.
void bodyclock_tts_set_streaming(int on);

// Worker threads for the NEXT bodyclock_tts_load(). <= 0 restores the engine's
// own choice.
//
// **Swept on the phone, and the count is not a lever**: four, six and eight sit
// within about 2% of each other, which is inside the ~9% a five-run sweep drifts
// from heat, and only two threads is clearly worse (12%). The engine's own
// choice of six costs nothing. It was added expecting the opposite.
//
// **Unlike bodyclock_local_set_threads, this one needs a reload**, because
// qwen.h has no thread parameter and the count is consumed once inside qt_init.
// After the first load that is about a second from page cache, and what is being
// compared is generation, which the reload does not touch.
void bodyclock_tts_set_threads(int threads);

// Streaming callback for the generated voice. `pcm` is mono float at 24000 Hz,
// the same rate the omni lane produces and not the 16 kHz of the input side.
//
// Return non-zero to keep going, 0 to abort. **`last` arrives on a call of its
// own with `n_samples` 0**, which differs from the omni handler: the engine's
// chunk callback carries no end marker, and giving it one would have meant
// holding each chunk back until the next proved it was not the last, delaying
// the first chunk. The first chunk arriving early is what the streaming path is
// for.
typedef int (*bodyclock_tts_audio_handler)(const float* pcm,
                                      int n_samples,
                                      int last,
                                      void* user_data);

// Speak `text`. BLOCKS until the whole utterance is generated.
//
// `on_audio` NULL generates and discards, which is a measurement mode rather
// than an oversight -- the difference between the two is the cost of the
// handover, not of the engine, and nothing here keeps a copy of the audio.
//
// Returns a JSON object, owned by the core and valid until this thread's next
// call, or NULL on failure (see bodyclock_tts_last_error()):
//
//   speechSeconds       seconds of audio produced.
//   seconds             seconds of computation.
//   realtime            the ratio, and the reason this lane exists. Above 1 it
//                       cannot talk as fast as it speaks. 1.16 on a desktop.
//   firstAudioSeconds   the wait before the first PCM arrives. 0 when buffered,
//                       where nothing arrived early at all.
//   chunks              handovers. 1 means nothing streamed.
//   streamed            which path this run took, which is not the same as what
//                       bodyclock_tts_set_streaming asked for: with no handler
//                       there is nobody to stream to.
//   peakDbfs            the loudest sample.
//   levelDriftDb        how much the delivery's level wanders over the
//                       utterance: the range of a four-second moving mean over
//                       its speech. A DESCRIPTION, with no threshold attached.
//                       One take was heard with a second half five to six dB
//                       louder than its first, which the model can do and the
//                       API cannot prevent -- but one labelled failure cannot
//                       set a cutoff, and a take nobody minded scored higher.
//                       Reported so the labels can accumulate.
const char* bodyclock_tts_say(const char* text,
                         bodyclock_tts_audio_handler on_audio,
                         void* user_data);

// Why the last load or say failed. "" after a success. Owned by the core.
const char* bodyclock_tts_last_error(void);

//------------------------------------------------------------------------------
// Model bundles
//------------------------------------------------------------------------------
//
// One file in, a directory out, so a model that is twenty files can be copied to
// a phone as one. Packed by design/pack-mnn-model.py; the format lives in
// src/platform/bundle.hpp.
//
// This is here for the omni lane, whose model MNN resolves as a directory
// relative to config.json -- but nothing in it is omni-specific, and it is
// available whether or not that engine was compiled in.

// What a bundle contains, without reading its payload: a JSON object with
// `files` (name and bytes each) and `totalBytes`. Cheap, so a host can show the
// contents and check for room before committing to several gigabytes of copying.
//
// Returns NULL when the file is missing, truncated, or not a bundle. The buffer
// is owned by the core and valid until this thread's next call.
const char* bodyclock_bundle_list(const char* bundle_path);

// Called as extraction runs. `fraction` is 0..1 over the whole payload.
// Return non-zero to continue, 0 to abort -- an abort removes what was written.
typedef int (*bodyclock_bundle_progress)(const char* name,
                                    double fraction,
                                    void* user_data);

// Extract into `dest_dir`, creating it. Returns 0 on success.
//
// **Either it all arrives or nothing does.** A failure removes what it wrote,
// because the alternative -- a truncated weight file -- is one MNN loads
// without complaint and then answers through.
//
// Entry names are validated: absolute paths, drive letters, backslashes and
// ".." anywhere cause the whole extraction to be refused. A bundle is a file the
// user copied from somewhere.
int bodyclock_bundle_extract(const char* bundle_path,
                        const char* dest_dir,
                        bodyclock_bundle_progress on_progress,
                        void* user_data);

#ifdef __cplusplus
}
#endif
