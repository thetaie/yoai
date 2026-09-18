// SenseVoiceSmall on ggml: audio in, text out, inside this process.
//
// Why recognition lives in the core rather than in each host app, and why it is
// a separate model instead of an omni one that would do this and the reply in a
// single pass, is argued in design/voice.md. The short version is that keeping
// recognition outside the language model leaves the model choice free -- voice
// then works against a remote provider too, which it cannot when the audio has
// to reach the weights.
//
// The model is a 70-layer SAN-M encoder plus a CTC head. No autoregression, so
// one forward pass produces the whole transcript and the cost is linear in the
// length of the clip rather than in the length of the answer.
//
// Licensing has two halves and they differ. This implementation derives from
// the SenseVoice repository's ggml runtime, which is MIT. The official
// SenseVoiceSmall WEIGHTS are not MIT: they carry FunASR's model open-source
// agreement, which permits commercial use but requires attribution and that the
// model be named. Shipping the weights therefore needs that attribution
// somewhere the user can reach it.
//
// Threading: one instance is NOT safe to call from two threads at once -- it
// owns a compute buffer that a turn writes through. Instances are independent.
// transcribe() blocks for the length of the pass, which is well under real time
// but not instant, so it does not belong on the thread that draws.

#pragma once

#include <string>
#include <vector>

namespace bodyclock {
namespace voice {

// One decoded token, kept rather than collapsed straight into a string.
//
// The frame index is the reason. Greedy CTC knows exactly which encoder frame
// emitted each token, and encoder frames are 60 ms, so a transcript carries
// usable timestamps at no extra cost. Prosodic segmentation wants them: a
// boundary confirmed by both a pause and a sentence end is better than one from
// either alone (design/voice.md).
struct Token {
    int         id    = 0;
    int         frame = 0;      // encoder frame that emitted it, audio-relative
    float       start = 0.0f;   // seconds into the clip, frame * 0.06
    std::string piece;          // the vocabulary entry, "\xe2\x96\x81" still marking word starts

    // The stray-capital filter removed this one, so it is not in `text`. Kept
    // rather than erased so that anything rebuilding text from a slice of the
    // tokens drops the same ones, and so a debug view can show what was taken.
    bool        dropped = false;
};

struct Transcript {
    std::string text;           // detokenized, tags removed, stray capitals filtered

    // The four query tokens the encoder answers alongside the transcript. Empty
    // when the model did not emit one. These are the model's own vocabulary,
    // passed through rather than mapped: "zh", "en", "yue", "ja", "ko",
    // "nospeech"; "HAPPY", "SAD", "ANGRY", "NEUTRAL", "FEARFUL", "DISGUSTED",
    // "SURPRISED"; "Speech", "BGM", "Applause", "Laughter", "Cry", and so on.
    //
    // The emotion label is deliberately NOT what this app reads emotion from.
    // Its label space has no word for tired, depleted or calm -- the whole
    // low-arousal band collapses into NEUTRAL -- so it is recorded and not
    // trusted. See design/voice.md.
    std::string language;
    std::string emotion;
    std::string event;
    bool        itn = false;    // the model applied inverse text normalization

    std::vector<Token> tokens;  // in emission order, tags included
    float audio_seconds = 0.0f;

    // Set when the stray-capital filter removed anything. A known failure mode
    // on some audio is a run of isolated capital letters interleaved into the
    // output; all precisions do it, including f32, so it is the model rather
    // than the quantization. It matters because a wrong transcription does not
    // look like silence, it looks like text -- and this text would otherwise
    // reach the language model as though the speaker had said it.
    bool filtered_stray_capitals = false;
};

class SenseVoice {
public:
    SenseVoice();
    ~SenseVoice();

    // Read a SenseVoiceSmall GGUF. BLOCKS while the weights are read and
    // mmapped -- 254 MB at q8 -- so call it off the drawing thread. Returns
    // false on a path that will not open or a file missing tensors this
    // architecture needs; the reason is logged.
    //
    // A second load on a loaded instance replaces the model.
    bool load(const std::string& gguf_path);
    bool loaded() const;

    // Inverse text normalization: with it on, a spoken "five hundred and
    // sixty-seven" comes back as "567". The GGUF asks for it on, and this
    // switches the query token that asks.
    //
    // It is not a cosmetic preference. **A transcript with ITN applied can no
    // longer be used to count syllables**, because "567" is three characters and
    // four spoken syllables -- and speaking rate, which design/voice.md takes
    // from the character count divided by the duration, is wrong at every number
    // in the clip. Anything measuring rate wants this off; anything showing text
    // to a person wants it on.
    //
    // Returns false when the model carries no textnorm query slot, in which case
    // nothing changed. Takes effect on the next transcribe().
    bool set_inverse_text_normalization(bool on);

    // Worker threads for the encoder. Takes effect on the next transcribe().
    //
    // **Default 4, and now measured** -- on a Kirin 9020, seconds of compute per
    // second of clip:
    //
    //     threads      1      2      4      6      8
    //     s per s    0.392  0.259  0.173  0.163  0.161
    //     vs one       1.0   1.51   2.27   2.40   2.43
    //
    // Four is where it stops paying. Eight buys 7% -- 0.07 s on a four-second
    // clip -- against two more cores taken from the UI, so 4 stays.
    //
    // The number 4 was originally inherited from the LLM lane and justified as
    // "the pass is memory-bound enough that a phone's little cores hold the
    // batch up rather than help". Both halves of that were wrong and the right
    // answer came out the same, which is the least useful way to be right. The
    // pass is a GEMM over ~80 stacked frames reusing every weight 80 times, and
    // the model is ~250 MB read once -- under a percent of this device's 24.4
    // GB/s. It is compute-bound, the little cores do contribute, and the
    // contribution is simply small.
    void set_threads(int n);

    // `pcm` is mono 16 kHz float in [-1, 1] -- what a microphone callback hands
    // over, so nothing on a device has to touch a file.
    //
    // Returns false when the model is not loaded or the clip is shorter than
    // one analysis window. A clip that is long enough but contains no speech is
    // NOT an error: it succeeds with empty text, usually with language set to
    // "nospeech", and the caller decides what that means.
    //
    // There is no segmentation here. One call is one pass over whatever is
    // handed in, and a recording long enough to need cutting should be cut
    // before it arrives -- by the VAD for recognition, or at pause boundaries
    // for analysis. Those are different cuts and must not be shared.
    bool transcribe(const float* pcm, size_t n_samples, Transcript* out);

    // Whether this build has the engine compiled in. When false, load() fails
    // and says so, and everything above still links -- the same shape as the
    // on-device LLM lane, so a host can ask before offering the affordance.
    static bool available();

private:
    SenseVoice(const SenseVoice&);
    SenseVoice& operator=(const SenseVoice&);

    // Keeps ggml out of this header, so a translation unit that only wants the
    // Transcript type does not need the SDK on its include path.
    struct Impl;
    Impl* impl_;
};

}  // namespace voice
}  // namespace bodyclock
