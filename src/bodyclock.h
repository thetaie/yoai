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
// This exists for a specific measurement. The claim it is there to settle is
// that decode is bandwidth-bound and one core already saturates the memory
// controller; if that is right, decode throughput is flat in this number, and
// if it rises through four or six cores then the wall is per-core rather than
// per-chip and several conclusions that lean on it have to be re-derived.
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
// Omni (MNN)
//------------------------------------------------------------------------------
//
// A SECOND engine, for the one thing the first cannot do: speak in the model's
// own voice. llama.cpp has no Talker checkpoint in GGUF; MNN has the whole chain
// -- thinker, talker, diffusion transformer, vocoder -- and it works. Measured
// on a desktop: audio in, a spoken answer out, in one call.
//
// It is NOT a replacement for anything. Recognition stays with SenseVoice, which
// is cheap and uniquely supplies the 60 ms alignment the clause cut needs; text
// stays with llama.cpp and its grammar; prosody is signal processing and needs no
// engine at all. This lane buys exactly one thing, and design/voice.md argues
// over whether that one thing is affordable. These entry points exist so the
// question can be answered on a phone instead of a workstation.
//
// Threading and cost: everything here BLOCKS, and generating speech measured
// about 3x slower than real time on a desktop CPU -- a fifteen-second reply is
// most of a minute. Call it off the drawing thread and expect to wait.

// Whether this build has the engine compiled in: 1 if yes, 0 if it was built
// without MNN. When 0 everything below still links and answers truthfully.
int bodyclock_omni_available(void);

// Read a model. BLOCKS.
//
// `model_dir` is a DIRECTORY, not a file -- and that is the difference from the
// other two lanes. An MNN model is config.json plus a dozen .mnn graphs and
// their weights (thinker, audio encoder, talker, predit, dit, bigvgan, speaker
// dictionary), about 3.8 GB together. A host whose file picker hands back single
// files has to pick the directory instead.
//
// Returns 0 when the model is resident.
int bodyclock_omni_load(const char* model_dir);

int bodyclock_omni_loaded(void);

// Drop the weights.
void bodyclock_omni_unload(void);

// Streaming callback for the generated voice, invoked as the waveform is
// produced. `pcm` is mono float at 24000 Hz -- note that this is NOT the 16 kHz
// the input side uses, so a host feeding it back to a speaker must say so.
//
// Return non-zero to keep going, 0 to abort. `last` is 1 on the final chunk.
typedef int (*bodyclock_omni_audio_handler)(const float* pcm,
                                       int n_samples,
                                       int last,
                                       void* user_data);

// Ask the model about one recording, and optionally have it answer aloud.
//
//   pcm, n_samples   16-bit signed mono at 16000 Hz, as with the voice lane.
//   prompt           what to ask about the audio. Required.
//   on_audio         NULL for a text-only answer. Non-NULL asks the Talker to
//                    speak the reply, which is most of the cost.
//
// Returns a JSON object, owned by the core and valid until this thread's next
// call, or NULL on a bad argument or an engine failure. It carries the reply
// `text` and the timings the decision in design/voice.md turns on:
//
//   speechRealtime      seconds of computation per second of speech, for the
//                       TALKER alone. Above 1 it cannot talk as fast as it
//                       speaks. Measured 3.7 on a phone.
//   firstAudioSeconds   the wait before the first PCM arrives, from the start
//                       of the exchange.
//   audioBlocks         how many handovers that took. 1 means nothing streamed.
//
// Those two were added to ask whether playing the reply as it arrives rescues
// the ratio. Measured: no. Nine blocks carried 10.34 s of speech over 38.5 s --
// one every 4.3 s holding 1.15 s of audio, so playback drains each in 1.15 s
// and then waits 3.2 s. 27% speech, 73% silence, which is 1 / 3.7. Streaming
// moves the first word much earlier and cannot close the gaps.
//   thinkerSeconds      audio in to last text token; `seconds` is everything.
//
// **Audio over about 30 seconds is refused rather than attempted.** Measured:
// 28 s works, 31 s segfaults inside the audio front end. Thirty seconds is the
// window every Whisper-family encoder has, so the limit is expected -- crashing
// instead of saying so is not, and the check here is what keeps that upstream
// bug out of the app.
const char* bodyclock_omni_ask(const short* pcm,
                          int n_samples,
                          const char* prompt,
                          bodyclock_omni_audio_handler on_audio,
                          void* user_data);

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
