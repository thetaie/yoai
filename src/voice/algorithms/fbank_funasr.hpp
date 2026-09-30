// The 80-bin log-mel front end SenseVoiceSmall was trained with, plus the
// low-frame-rate stacking that follows it.
//
// Faithfulness matters more than elegance here. The encoder was trained on the
// exact output of FunASR's front end, and a feature pipeline that is merely
// close does not degrade gracefully -- it produces text that is confidently
// wrong, which is the failure this project keeps trying to design out. Every
// constant below is therefore the trained one, and the places where this
// differs from a textbook mel filterbank are commented rather than tidied away.
//
// No ggml and no model: this is arithmetic over a buffer of samples, so it
// compiles and can be checked on any platform whether or not the ASR lane is
// built. The FFT is in fft.hpp; everything else here is FunASR's convention
// and stays with it.
//
// It is scalar on purpose, and should stay that way.
//
// **Measured** on the desktop, `q8`, a 32-second clip: the front end takes about
// 21 ms and does not move with the encoder's thread count, so its share of a
// pass runs from 0.4% on one thread to 2.2% on sixteen. Twenty-one milliseconds
// for thirty-two seconds of audio is already 1500x real time; it is not on the
// critical path at any thread count, and the encoder is 98% of the work. NEON or
// AVX here would buy one or two percent. (On a phone the share is somewhat
// higher -- ggml's kernels get `i8mm` and `dotprod` from this repo's -march
// flags while this stays scalar -- but a few percent of a few percent is still
// nothing.)
//
// The reason not to do it anyway is the one that matters: **being exactly right
// is this file's entire job, and bit-level agreement with the reference
// implementation is how that is known.** A real-input FFT, a different twiddle
// schedule or a vectorized mel projection would each change the last bits, and
// the failure they risk does not announce itself -- the encoder answers
// confidently on features that are slightly wrong. Trading a verified property
// for one percent is a bad trade.
//
// If the front end ever does need to be cheaper, the first move is not SIMD: it
// is to hoist the three per-frame buffers in mel_frame() into members, which is
// allocation-free and changes no arithmetic at all.

#pragma once

#include <cstddef>
#include <vector>

namespace yoai {
namespace voice {

// Defaults are SenseVoiceSmall's. They are exposed because a second model on
// the same front end (Fun-ASR-Nano shares the SAN-M encoder) may differ in the
// stacking, not so that they can be tuned.
struct FunAsrFbankOptions {
    int   sample_rate  = 16000;
    int   frame_length = 400;      // 25 ms
    int   frame_shift  = 160;      // 10 ms
    int   n_fft        = 512;      // first power of two above frame_length
    int   n_mel        = 80;
    float low_freq     = 20.0f;
    float high_freq    = 8000.0f;
    float preemphasis  = 0.97f;

    // Low frame rate: every output frame is `lfr_m` consecutive mel frames laid
    // end to end, and the window advances by `lfr_n`. With 7 and 6 the encoder
    // sees one frame per 60 ms carrying 115 ms of context, which is what makes a
    // 70-layer encoder affordable on a phone.
    int   lfr_m        = 7;
    int   lfr_n        = 6;
};

class FunAsrFbank {
public:
    explicit FunAsrFbank(const FunAsrFbankOptions& opt = FunAsrFbankOptions());

    // `pcm` is mono, at opt.sample_rate, nominally in [-1, 1].
    //
    // Returns the features row-major, `*frames` rows of dim() floats. An input
    // shorter than one analysis window yields no rows rather than a padded one:
    // a caller with 10 ms of audio should be told there is nothing to recognize,
    // not handed a frame of silence-shaped noise.
    std::vector<float> compute(const float* pcm, size_t n, int* frames) const;

    // Width of one output row: lfr_m * n_mel, 560 for SenseVoiceSmall.
    int dim() const { return opt_.lfr_m * opt_.n_mel; }

    // How many output rows `n` samples will produce, without computing them.
    // Used to size buffers and to decline a clip up front.
    int frames_for(size_t n) const;

    const FunAsrFbankOptions& options() const { return opt_; }

private:
    // One frame of samples to n_mel log-energies. Separate so the mel stage can
    // be checked on its own against a reference frame.
    void mel_frame(const float* samples, float* out) const;

    FunAsrFbankOptions opt_;
    std::vector<float> window_;      // frame_length
    std::vector<float> mel_weights_; // n_mel * n_bins, mostly zero, dense anyway
    std::vector<int>   mel_begin_;   // first non-zero bin per filter
    std::vector<int>   mel_end_;     // one past the last, so the inner loop is short
    int                n_bins_;      // n_fft / 2 + 1
};

}  // namespace voice
}  // namespace yoai
