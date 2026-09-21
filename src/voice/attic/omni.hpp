// Qwen2.5-Omni on MNN: audio in, an answer out, optionally spoken.
//
// The second engine in this core, and it earns its place with exactly one
// capability: **the model's own voice**. llama.cpp ships no Talker checkpoint in
// GGUF, so on that lane a reply can only be read aloud by the platform. MNN
// carries the whole chain -- thinker, talker, diffusion transformer, vocoder --
// and produces PCM.
//
// Everything else it could do is already done better elsewhere in this core, and
// that is deliberate rather than grudging:
//
//   - Recognition: SenseVoice is a fraction of the size and is the only thing
//     here that yields 60 ms token alignment, which is what the clause cut runs
//     on.
//   - Text axes: llama.cpp, with a grammar, decoding greedily.
//   - Arousal: signal processing, no engine.
//
// So this file is narrow on purpose. It does not transcribe, it does not score,
// and it must never become the place those things happen -- design/voice.md
// records what each of them costs and why they live where they do.
//
// What is NOT settled is whether the one capability is affordable. Measured on a
// desktop CPU, generating speech runs about 3x slower than real time. This class
// exists so that number can be taken on a phone.

#pragma once

#include <cstddef>
#include <string>

namespace bodyclock {
namespace voice {

// What one exchange produced.
struct OmniReply {
    std::string text;

    // Seconds of speech the Talker generated, 0 when it was not asked to speak.
    float speech_seconds = 0.0f;

    // Wall clock, split because one ratio hid the question. speech_elapsed
    // covers the Talker alone, so speech_elapsed / speech_seconds is the figure
    // design/voice.md argues from -- but a person waits `seconds`, and the
    // difference between the two is the thinker.
    //
    // None of these are processor time. Timed with clock() the Talker reported
    // 87.9 s for 18.2 s of work, because MNN runs four threads and clock() sums
    // them; a ratio built on that decides the wrong way.
    double seconds        = 0.0;
    double thinker_seconds = 0.0;
    double speech_elapsed = 0.0;

    // When the FIRST block of PCM arrived, from the start of the exchange, and
    // how many blocks there were. 0 when nothing was spoken.
    //
    // Added to ask whether streaming rescues the ratio. **Measured: it does
    // not**, and the reason is worth having in one line. Nine blocks carried
    // 10.34 s of speech over 38.5 s: a block every 4.3 s holding 1.15 s of
    // audio. Playback drains one in 1.15 s and then waits 3.2 s -- **27%
    // speech, 73% silence**, which is the ratio itself (1 / 3.7).
    //
    // So streaming moves the first word much earlier (14.7 s against 47.7 s)
    // and cannot close the gaps. Above 1x the listener always outruns the
    // generator; below 1x the gaps close for free.
    //
    // They stay because they are what that conclusion rests on, and because
    // they become the interesting pair the day the ratio goes below 1.
    double first_audio_seconds = 0.0;
    int    audio_blocks        = 0;
};

// The audio the Talker produced, as it is produced. `pcm` is mono float at
// **24000 Hz** -- not the 16 kHz of the input side, which is a difference worth
// carrying in the type's documentation because nothing in the signal itself
// announces it. Return false to abort generation.
typedef bool (*OmniAudioSink)(const float* pcm, size_t n_samples, bool last, void* user_data);

class Omni {
public:
    Omni();
    ~Omni();

    // Whether this build has MNN compiled in.
    static bool available();

    // Read a model from a DIRECTORY -- config.json plus a dozen graphs and their
    // weights, about 3.8 GB. BLOCKS for the length of the load.
    //
    // Unlike the GGUF lanes, which take a single file, this needs the whole
    // directory: the thinker, audio encoder, talker, predit, dit, bigvgan and
    // speaker dictionary are separate files that MNN resolves relative to
    // config.json.
    bool load(const std::string& model_dir);
    bool loaded() const;

    // Solver steps for the diffusion vocoder. Takes effect on the next ask; the
    // weights stay resident. False with no model loaded, or below 2.
    //
    // MNN runs `steps - 1` DiT forwards per chunk, so 5 is four and 2 is one.
    // It is the only lever on this lane's 3.7x that does not need a different
    // model -- and it trades exactly what the lane is for, because a coarser
    // sampler is a less natural voice. See the note in src/bodyclock.h.
    bool set_dit_steps(int steps);

    // Ask about one recording.
    //
    // `pcm` is mono float at 16 kHz in [-1, 1], which is what the microphone
    // hands over -- it goes in from memory, with no temporary file on disk. That
    // matters beyond tidiness: a voice note that never reaches the filesystem
    // cannot be left behind by a crash.
    //
    // `sink` NULL asks for text only. Non-NULL asks the Talker to speak the
    // reply, which is most of the cost.
    //
    // Refuses audio longer than about 30 seconds rather than attempting it.
    //
    // **The number is not a limit, and the thing it guards against is not a
    // threshold.** It was set from two runs -- 28 s fine, 31 s segfault --
    // which read as a boundary. Re-measured later on a PC through MNN's own
    // llm_demo, the same clip at the same length both crashes and succeeds:
    // 18 s went crash, ok, crash, crash, and 26 s went ok, crash, crash, crash.
    // A length limit cannot do that, and the bisect had already said so by
    // passing 26 s after failing 18 and 22.
    //
    // Nor is it a thread race: one thread crashes too.
    //
    // So this guard does not prevent the crash. It shortens the input, which
    // does less work and gives a memory bug in MNN's audio front end fewer
    // chances to fire. That is worth having and it is not what the name says.
    // design/voice.md has the runs.
    bool ask(const float* pcm, size_t n_samples, const std::string& prompt,
             OmniAudioSink sink, void* user_data, OmniReply* out);

    // The longest clip ask() will accept, in seconds.
    static float max_seconds();

private:
    Omni(const Omni&);
    Omni& operator=(const Omni&);

    struct Impl;
    Impl* impl_;
};

}  // namespace voice
}  // namespace bodyclock
