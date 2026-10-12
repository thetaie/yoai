// Acoustic echo cancellation: take what the speaker played out of what the
// microphone heard, so that someone talking over a reply can still be
// recognized.
//
// Why the core carries its own rather than asking the platform: a platform's
// canceller references only its call path. Android's cancels a playback stream
// only when that stream is marked as voice communication, which routes it --
// volume, processing -- as a call; HarmonyOS's comes with capturing for voice
// communication; and whether Chromium's covers what a page plays through Web
// Audio is unsettled. A smart-speaker reply played as media falls outside all
// three.
//
// This is the linear stage: a partitioned-block frequency-domain adaptive
// filter (MDF) that learns the path from the reference to the microphone --
// output buffering, loudspeaker, room, input buffering -- and subtracts its
// estimate of the echo. Its step size follows J.-M. Valin, "On adjusting the
// learning rate in frequency domain echo cancellation with double-talk" (IEEE
// TASLP, 2007): the step is the residual echo's share of the error, estimated
// per bin from how the error's power tracks the echo estimate's. When someone
// talks over the echo, the error grows without the echo estimate growing with
// it, the share falls, and adaptation slows by itself -- no separate
// double-talk detector.
//
// What it does not do yet, each measured before it is added:
//  - Residual echo suppression. A linear filter leaves what it cannot model --
//    loudspeaker distortion, a path that just moved -- and recognition
//    transcribes echo even 20 dB down when nothing else is in that stretch.
//  - Delay estimation. The filter must span the whole path, bulk delay
//    included, so a delay longer than `filter_ms` is not cancelled at all.
//  - A reference and a capture on two threads. process() takes both in
//    lockstep; pairing a playback callback with a capture callback is a layer
//    above this one.
//
// Not thread-safe; one caller.

#pragma once

#include <cstddef>
#include <vector>

namespace yoai {
namespace voice {

struct EchoCancellerOptions {
    int sample_rate = 16000;

    // Samples per block, a power of two. The output lags by at most one block,
    // and a smaller block adapts more often for more FFTs a second. 256 is
    // 16 ms at 16 kHz.
    int block = 256;

    // The longest echo path the filter can model, bulk delay included, rounded
    // up to whole blocks. Room reverberation that matters is 100 to 300 ms and
    // device buffering adds its own; the cost is linear in this.
    int filter_ms = 256;
};

class EchoCanceller {
public:
    explicit EchoCanceller(const EchoCancellerOptions& opt = EchoCancellerOptions());

    // `mic` and `ref` are the same length and in lockstep: ref[i] is what was
    // sent to the speaker at the moment mic[i] was captured, so the echo of
    // ref[i] reaches the microphone at or after i. A reference that arrives
    // later than its own echo cannot be cancelled.
    //
    // Appends to `out` the echo-removed microphone, one sample for each sample
    // of `mic`, in whole blocks: out's i-th sample is mic's i-th with the echo
    // taken out, available once its block is complete.
    void process(const float* mic, const float* ref, size_t n, std::vector<float>* out);

    // Forget the learned path and anything buffered.
    void reset();

    // What the adaptation currently believes, for a test or a debug page.
    struct Stats {
        long   blocks  = 0;       // processed since reset
        bool   adapted = false;   // past the bootstrap, on Valin's step size
        double leak    = 1.0;     // estimated leakage: residual echo / echo estimate, in power
        int    resets  = 0;       // times the filter diverged and was thrown away
    };
    const Stats& stats() const { return stats_; }

    const EchoCancellerOptions& options() const { return opt_; }
    int partitions() const { return m_; }

private:
    void process_block(const float* mic, const float* ref, float* out);
    void clear_filter();

    // Real input of fft_size_ samples to bins 0..block_ (the rest are the
    // mirror image), and back.
    void forward(const float* time, float* re, float* im);
    void inverse(const float* re, const float* im, float* time);

    EchoCancellerOptions opt_;
    int n_        = 0;   // block
    int fft_size_ = 0;   // 2 * block
    int bins_     = 0;   // block + 1
    int m_        = 0;   // partitions

    // Pending input, less than a block of each.
    std::vector<float> mic_in_, ref_in_;

    // The reference: the previous block, and the spectra of the last m_
    // two-block windows, newest at x_head_. Partition p of the filter meets the
    // window p blocks back.
    std::vector<float> ref_prev_;
    std::vector<float> x_re_, x_im_;   // m_ * bins_ each
    int x_head_ = 0;

    // The filter, partition-major like the reference spectra.
    std::vector<float> w_re_, w_im_;

    // Adaptation state; see process_block for each.
    std::vector<float> e_mean_, y_mean_;  // bins_, slow means of the error and echo spectra
    double pey_ = 1.0, pyy_ = 1.0;        // smoothed covariance, for the leakage
    double sum_adapt_ = 0.0;
    int    diverging_ = 0;

    // The microphone's DC, removed before anything else.
    float dc_in_ = 0.0f, dc_out_ = 0.0f;

    // Scratch, sized once.
    std::vector<float> t_, re_, im_, f_re_, f_im_;

    Stats stats_;
};

}  // namespace voice
}  // namespace yoai
