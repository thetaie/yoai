// Qwen3-TTS VoiceDesign on qwentts.cpp: text in, a described voice out.
//
// The THIRD engine in this core, and like the second it is here for one thing.
// omni.hpp explains what MNN buys -- the model's own voice -- and design/voice.md
// records what it costs: 2.8x slower than real time at the coarsest diffusion
// setting that still sounds like anything, on a chain that also has to think
// before it speaks. This engine speaks text somebody else produced, so the
// thinker is llama.cpp's and already measured, and the speaking measured 1.16x
// on a desktop against omni's 3.0x for the same job.
//
// **The voice is written, not cloned.** `instruct` is an English description --
// `breathy, unhurried but not slow` -- and the same description gives the same
// voice across different passages. That is the requirement the Base checkpoint
// failed: free-running it changed speaker between texts, including gender, and
// pinning it through the speaker encoder held the identity but flattened the
// prosody. VoiceDesign holds both, and needs no reference recording shipped in
// the app or captured from the user.
//
// Two files, both GGUF: a talker and a 12 Hz audio tokenizer. The 1.7B
// VoiceDesign pair is 1.4 GB at Q4_K_M; there is no smaller VoiceDesign.
//
// **The 0.6B CustomVoice pair is 0.8 GB and about a tenth faster**, and it does
// not give up the written voice: that checkpoint takes a named speaker AND an
// optional style instruction, so identity comes from a table of nine and
// delivery is still described. Only the talker stage shrinks with it -- the code
// predictor is sized by the codebook count, and the codec is a separate model --
// so a third of the parameters buys 14% of a frame. See set_speaker().
//
// **On the phone it has TWO speeds**: 2.14x for the first half minute and 3.52x
// after, on the 1.7B; 1.94x and 3.30x on the 0.6B. The step is a clock step,
// flat on either side of a 65% jump, and **when it arrives depends on thermal
// history and not at all on model size** -- about 30 s from cold, about 17 s
// after a minute of full load. Short measurements never leave the fast regime,
// so a ratio taken from one is a sprint figure; a 30-second passage takes 60 to
// 97 seconds.
//
// Of the 1.7B's fast 2.14x, **0.92 is the codec alone** -- 73.5 ms per 80 ms
// frame, unchanged by streaming, by halving the talker's weights, by quantizing
// itself, and by a talker a third the size. Add the code predictor, which is
// sized by the codebook count rather than the LM, and **1.45x is there with a
// talker costing nothing**. So the ceiling is not a tuning problem, and nothing
// here should be written as though the next lever will cross 1x.
// design/voice.md has all of it.
//
// Stubs out without BODYCLOCK_QTTS, exactly like omni: available() is false,
// load() fails with a message, and the app still builds and links.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace bodyclock {
namespace voice {

// What one synthesis produced. Every duration is wall clock from a steady clock
// -- see the note in omni.hpp for why that is spelled out.
struct QttsResult {
    // Seconds of audio, at 24000 Hz.
    float speech_seconds = 0.0f;

    // Seconds of computation. `elapsed / speech_seconds` is the ratio the port
    // exists to measure; above 1 the engine cannot talk as fast as it speaks.
    double elapsed = 0.0;

    // When the first PCM arrived, from the call. Streaming only; 0 in the
    // buffered path, where the first and only handover is at the end.
    //
    // Worth its own number even though the ratio decides affordability: below
    // 1x the gaps close on their own and this is the entire remaining wait.
    double first_audio_seconds = 0.0;

    // Handovers to the sink. 1 means nothing streamed.
    int chunks = 0;

    // Loudest sample, in dBFS. 0 when nothing was produced.
    float peak_dbfs = 0.0f;

    // **How much the delivery's LEVEL wanders over the utterance**, in dB: the
    // range of a four-second moving mean taken over the speech windows only.
    // 0 when the audio is too short to have two such windows.
    //
    // A description, not a verdict, and deliberately not a threshold. It is
    // here because one generated take had a second half five to six decibels
    // louder than its first, which the model can do and nothing in the API
    // prevents -- the seed is a free draw and occasionally draws that.
    //
    // **The obvious statistic did not work.** The largest jump between adjacent
    // windows was tried first and ranks the bad take in the MIDDLE of takes
    // nobody complained about: ordinary speech steps 10 to 15 dB between
    // half-second windows at every syllable and phrase boundary, so a defect
    // that is a sustained offset between halves is invisible to it.
    //
    // This one is directionally right on the few takes that have been
    // judged -- 2.0 dB on two that sounded fine against 3.9 on the one that did
    // not -- and **it is not validated**: a fourth take that sounded fine scored
    // 4.5. One labelled failure cannot set a cutoff. What would: a dozen takes
    // across seeds, each listened to and marked, which is an afternoon and has
    // not been done.
    //
    // Computed from the samples as they pass, so nothing here keeps a copy of
    // the audio.
    float level_drift_db = 0.0f;
};

// Generated audio, as it is generated. `pcm` is mono float at **24000 Hz** --
// the same rate omni produces and not the 16 kHz the microphone side uses.
// Return false to abort.
//
// **Called from qwentts.cpp's compute worker, not from the say() caller.** It
// must be safe there, it must not re-enter this class, and a slow body stalls
// generation -- hand the samples to a queue and return.
//
// **In STREAMING mode `last` arrives on a call of its own, with `n_samples`
// 0**, and that is where this differs from the omni sink, which sets it on the
// final block of real audio. qwentts.cpp's chunk callback carries no end
// marker, so the alternative was to hold each chunk back until the next one
// proved it was not the last -- which would delay the first chunk, and the
// first chunk arriving early is the entire reason streaming exists. The empty
// call is sent once say() has succeeded; an aborted or failed synthesis sends
// none.
//
// In buffered mode there is one call, carrying everything, with `last` set.
typedef bool (*QttsAudioSink)(const float* pcm, size_t n_samples, bool last, void* user_data);

class Qtts {
public:
    Qtts();
    ~Qtts();

    // Whether this build has qwentts.cpp compiled in.
    static bool available();

    // Read the two GGUFs. BLOCKS for the length of the load, about 2.2 GB.
    //
    // `talker_path` holds the LM weights and the code predictor; `codec_path`
    // holds the 12 Hz audio tokenizer. Both are required and they are not
    // interchangeable, so a swapped pair fails here rather than producing noise.
    //
    // See set_threads() for the one thing about this load that is not obvious.
    bool load(const std::string& talker_path, const std::string& codec_path);
    bool loaded() const;
    void unload();

    // What the loaded talker is: "voice_design", "base" or "custom_voice",
    // empty with nothing loaded.
    //
    // Exposed because the three take DIFFERENT parameters and the checkpoint
    // decides which -- an `instruct` is required by voice_design and refused by
    // base. A host that lets someone pick a file needs to be able to say which
    // one they picked.
    std::string model_type() const;

    // The voice, described. Takes effect on the next say(); the weights stay
    // resident, so a sweep compares descriptions rather than page caches.
    //
    // Empty is not neutral, it is invalid on a voice_design checkpoint, and
    // say() reports that rather than inventing a default.
    //
    // On a custom_voice checkpoint it is OPTIONAL and combines with
    // set_speaker(): the speaker fixes who is talking, this describes how. On a
    // base checkpoint the engine refuses it outright.
    void set_voice(const std::string& instruct);
    const std::string& voice() const;

    // The named speakers a custom_voice checkpoint carries, in the order the
    // model lists them. Empty for the other two kinds, and empty before a load.
    //
    // Read from the model rather than hard-coded, because "nine speakers" is
    // something this repo learned from a model card and has never verified, and
    // a list that came from the file cannot be stale.
    std::vector<std::string> speakers() const;

    // Which of them speaks. Takes effect on the next say().
    //
    // **Only legal on a custom_voice checkpoint**, and say() sends it only
    // there, so a name left over from an earlier model cannot break a
    // VoiceDesign run. Empty asks the engine for its own default, which on this
    // checkpoint is a speaker rather than nothing.
    void set_speaker(const std::string& name);
    const std::string& speaker() const;

    // Whether to emit audio as it is generated. On by default, and **measured
    // to cost nothing**.
    //
    // Streaming moves the first audio from the end of the run to about a second
    // -- 930 to 1109 ms over four runs, against 8.5 to 9.6 s buffered. Chunks
    // start at one 12.5 Hz frame and widen to eight.
    //
    // The switch exists because that looked like it should cost something. The
    // codec is the one stage that did not scale like the others between desktop
    // and phone, it is also the one stage that ran differently between them, and
    // the streaming path really does re-decode the codec's left context per
    // chunk. Seven runs on a pinned seed, alternating: buffered is not faster.
    // Its best codec pass is 3073 ms against streaming's 3013, and its worst is
    // the worst of the seven.
    //
    // What that measurement did find is that the codec WANDERS -- 3013 to 3997
    // across two minutes on identical input, where five back-to-back runs of one
    // configuration later held inside 1%. It is drift, not scatter, so a
    // comparison is worth having only when its two halves run close together.
    // The switch stays as the thing that has to be held fixed before any claim
    // about this stage means anything.
    //
    // Streaming does not make playback smooth, and nothing here claims it does:
    // above 1x the listener drains each chunk faster than the next arrives.
    void set_streaming(bool on);
    bool streaming() const;

    // Worker threads for the NEXT load(). <= 0 restores upstream's own choice.
    //
    // **Measured, and the answer is that it does not matter.** Four, six and
    // eight land within about 2% of each other on the phone -- inside the drift
    // a five-run sweep accumulates from heat -- and only two threads is clearly
    // worse, by 12%. qwentts.cpp's own choice of six costs nothing.
    //
    // It was added on a suspicion that reads well and was wrong: the engine
    // picks hardware_concurrency() / 2 reasoning about hyperthreads, this phone
    // has none, and the llama lane on the same device had measured prefill and
    // decode flat from four threads up. All true, and none of it implied a
    // penalty. The argument predicted a difference; the sweep found none.
    //
    // It stays because a different device may answer differently and because
    // the alternative is trusting the same argument again. **A sweep here
    // RELOADS**, unlike the llama lane's: the count is consumed once, when
    // qt_init builds the backend and hands it to ggml_backend_set_n_threads.
    // After the first load that costs about a second from page cache.
    //
    // Carried by an environment variable, because qwen.h has no thread
    // parameter; harmony/patches/ is what teaches qwentts.cpp to read it, and
    // without that patch this is a no-op.
    void set_threads(int n);

    // A recording to sound like, and what is said in it. For a `base`
    // checkpoint, which is the cloning one and takes neither a description nor
    // a named speaker.
    //
    // **The WAV must be mono, 16-bit, exactly 24000 Hz**, and a file that is not
    // is refused rather than converted. That is the same rule the recognition
    // lane states for 16 kHz and it is there for the same reason: a resampler
    // that differs from the one a measurement was taken with changes the answer
    // quietly, and this project has been bitten by that before.
    //
    // Both arguments are needed together. The transcript is what makes this
    // in-context cloning rather than a speaker-embedding copy, and that is the
    // difference between the two rows of design/voice.md's variant table that
    // sound alike and the two that do not -- pinning a voice through the speaker
    // encoder held the identity and flattened the prosody; carrying the audio
    // and its text in context held both.
    //
    // **It is not free.** The whole reference sits in the talker's context for
    // every frame generated, so a 22-second clip is 280 frames of attention on
    // top of each step. On the desktop that measured 70% dearer than a described
    // voice. A shorter reference is cheaper and has not been tried.
    //
    // Returns false with last_error() set if the file will not load.
    bool set_reference(const std::string& wav_path, const std::string& transcript);
    void clear_reference();

    // Seconds of reference currently held, 0 when none.
    float reference_seconds() const;

    // Sampling. -1 seed draws a fresh one per call.
    //
    // On the Base checkpoint the seed WAS the speaker -- two seeds gave a man
    // and a woman reading the same sentence. It is not that here: the
    // description pins the identity and the seed only varies the delivery. That
    // is the whole difference between the two checkpoints, and it is why this
    // setter is safe to expose where on Base it would have been a trap.
    void set_seed(long long seed);
    void set_top_k(int top_k);

    // Speak `text`. BLOCKS until the whole utterance is generated.
    //
    // `sink` receives the audio, in chunks or in one piece depending on
    // set_streaming(). **`sink` NULL generates and discards**, which is not an
    // oversight: it is the same run without a consumer, so the difference
    // between the two is the cost of the handover rather than of the engine. It
    // is a measurement mode, and there is no other way to get the audio -- this
    // class never keeps a copy.
    //
    // Returns false on a failure, with last_error() set. A sink that returned
    // false is NOT a failure: generation stops early and this returns true with
    // whatever was produced, the same way the host cancels on every other lane.
    bool say(const std::string& text, QttsAudioSink sink, void* user_data, QttsResult* out);

    // Why the last load() or say() returned false. Empty after a success.
    const std::string& last_error() const;

    // The rate every sink receives, and what a player has to be told.
    static int sample_rate();

private:
    Qtts(const Qtts&);
    Qtts& operator=(const Qtts&);

    struct Impl;
    Impl* impl_;
};

}  // namespace voice
}  // namespace bodyclock
