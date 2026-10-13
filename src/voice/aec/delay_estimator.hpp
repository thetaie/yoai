// Where the echo is: how far behind the reference the microphone hears it.
//
// The echo canceller models the path from what was played to what was heard
// only across its filter's span, 256ms by default, and the bulk delay --
// output buffering, input buffering, a Bluetooth link -- spends that span on
// nothing. Past it the filter learns a spurious correlation and adds echo
// rather than removing it: with 300ms of buffering in a synthetic room the
// output came out 6.6dB louder than the echo. This finds the bulk delay, so
// the reference can be shifted by it before the canceller sees it.
//
// It listens to what is being played anyway rather than playing a probe. A
// probe at start-up is audible, and it is out of date the first time the
// output stream is reopened, the device changes or a buffer underruns. The
// reply is a probe that is there whenever there is echo to cancel.
//
// The method is GCC-PHAT: the cross-spectrum of a window of the reference and
// of the microphone, each bin divided by its own magnitude so that every
// frequency votes equally, back to time, and the lag of the peak. Speech is
// far from white, and without the division its low harmonics would spread the
// peak into a hump. Only 100Hz to 7kHz vote: below is room modes and the
// canceller's DC notch, above is where the resampled reference is empty.
//
// A window's answer is where the echo starts: the earliest lag reaching half
// the highest peak, since a reflection can whiten to a higher peak than the
// direct path ahead of it. A window votes only when its peak stands well above
// the correlation's RMS -- with no echo in the microphone, headphones for
// instance, it does not -- and a delay is accepted only when `agree` voting
// windows in a row give the same one. Reflections make the highest peak a poor
// witness on its own: in a synthetic room the next-highest was within 1/1.4
// to 1/1.1 of it. The thresholds and what they were measured on are in the
// .cpp.
//
// Not thread-safe; one caller. process() takes both signals in lockstep, the
// same contract as EchoCanceller.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace yoai {
namespace voice {

struct DelayEstimatorOptions {
    int sample_rate = 16000;

    // Reference per estimate. Measured on speech in a synthetic room: 1s found
    // 30ms and 300ms to the sample, 0.5s was 5.7ms off at 300ms, 0.25s wrong
    // at both.
    int window_ms = 1000;

    // The longest delay searched. The microphone has to be held this much
    // longer than the reference window, so the first estimate needs
    // window_ms + max_delay_ms of audio.
    int max_delay_ms = 500;

    // How often a window is estimated. Consecutive windows overlap by
    // window_ms - hop_ms, which is not independent evidence, so this and
    // `agree` together decide how hard a spurious peak finds it to be accepted.
    int hop_ms = 500;

    // Voting windows in a row that must agree, and how closely, in samples.
    int agree     = 3;
    int tolerance = 2;
};

class DelayEstimator {
public:
    explicit DelayEstimator(const DelayEstimatorOptions& opt = DelayEstimatorOptions());

    // `mic[i]` was captured when `ref[i]` was sent to the speaker, as for
    // EchoCanceller::process. Estimates run at every hop boundary, so the
    // result is the same however the input is split.
    void process(const float* mic, const float* ref, size_t n);

    // The accepted delay in samples, or -1 before there is one.
    int  delay() const { return delay_; }
    bool has_delay() const { return delay_ >= 0; }

    // Start from a known delay -- the one measured last time on this output
    // device, say -- until the windows agree on another. Not counted as a
    // change.
    void set_delay(int samples);

    // Forget everything, a delay set with set_delay() included.
    void reset();

    struct Stats {
        long   windows    = 0;    // estimated
        long   skipped    = 0;    // too little reference playing to estimate
        long   weak       = 0;    // of `windows`, no peak that stands out: no vote
        int    last       = -1;   // the latest window's own lag in samples, -1 if weak
        double peak_ratio = 0.0;  // its peak over the highest elsewhere
        double peak_snr   = 0.0;  // its peak over the correlation's RMS, every lag
        int    changes    = 0;    // times the accepted delay moved
    };
    const Stats& stats() const { return stats_; }

    const DelayEstimatorOptions& options() const { return opt_; }
    int window() const { return window_; }
    int max_lag() const { return max_lag_; }

private:
    void estimate();
    bool active(const float* ref) const;

    DelayEstimatorOptions opt_;
    int window_  = 0;   // samples
    int max_lag_ = 0;
    int hop_     = 0;
    int fft_n_   = 0;
    int bin_lo_  = 0, bin_hi_ = 0;

    // The newest window_ + max_lag_ samples of each, oldest first, and a little
    // more between trims.
    std::vector<float> ref_hist_, mic_hist_;
    int64_t total_ = 0;

    std::vector<int> recent_;   // raw lags of the latest windows, oldest first
    int delay_ = -1;

    // Scratch, sized once.
    std::vector<float> a_re_, a_im_, b_re_, b_im_;

    Stats stats_;
};

}  // namespace voice
}  // namespace yoai
