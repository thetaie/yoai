#include "voice/algorithms/delay_estimator.hpp"

#include "voice/algorithms/fft.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace yoai {
namespace voice {
namespace {

// The band that votes; see the header.
const double kLowHz  = 100.0;
const double kHighHz = 7000.0;

// A reference window is estimated only when at least half its 20ms frames are
// above -50dBFS. Silence has no phase to line up, and whitening it would give
// the noise floor a full vote.
const double kActivePower  = 1e-5;
const double kActiveShare  = 0.5;

// Around the peak, this many samples either side are its own width rather than
// a competitor when the ratio to the next-highest is taken. 1ms.
const int kPeakHalfWidthMs = 1;

// A window's lag is the earliest one reaching this share of the highest peak,
// not the highest peak itself. What the canceller needs is where the echo
// starts; a reflection can whiten to a higher peak than the direct path ahead
// of it, and then the highest peak flips between the two from window to
// window. Measured in a synthetic room with a reflection 5.7ms behind the
// direct path: taking the highest, the accepted delay moved five times in 8s
// and ended on the reflection.
const float kOnsetShare = 0.5f;

// Against the correlation's RMS over every lag searched. Measured on speech in
// a synthetic room, 112 windows with echo read 7.2 to 16.1, median 13; 56
// windows of the reply against an unrelated passage read 3.5 to 6.2, where
// the highest of 8000 Gaussian lags would sit near 4.2. A window counts only
// when its peak reaches kMinPeakSnr, and the onset must itself reach
// kMinOnsetSnr: the earliest lag above half the peak is otherwise the earliest
// noise, which lands near lag 0 in window after window and agrees with itself.
const double kMinPeakSnr  = 8.0;
const double kMinOnsetSnr = 6.5;

int next_pow2(int v) {
    int p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

}  // namespace

DelayEstimator::DelayEstimator(const DelayEstimatorOptions& opt) : opt_(opt) {
    if (opt_.sample_rate <= 0) {
        opt_.sample_rate = 16000;
    }
    if (opt_.window_ms <= 0) {
        opt_.window_ms = 1000;
    }
    if (opt_.max_delay_ms <= 0) {
        opt_.max_delay_ms = 500;
    }
    if (opt_.hop_ms <= 0) {
        opt_.hop_ms = 500;
    }
    opt_.agree     = std::max(1, opt_.agree);
    opt_.tolerance = std::max(0, opt_.tolerance);

    window_  = static_cast<int>(static_cast<long>(opt_.window_ms) * opt_.sample_rate / 1000);
    max_lag_ = static_cast<int>(static_cast<long>(opt_.max_delay_ms) * opt_.sample_rate / 1000);
    hop_     = std::max(1, static_cast<int>(static_cast<long>(opt_.hop_ms) * opt_.sample_rate / 1000));
    // Room for every lag 0..max_lag_ without the circular correlation wrapping:
    // the reference is window_ long, the microphone window_ + max_lag_.
    fft_n_  = next_pow2(window_ + max_lag_);
    bin_lo_ = static_cast<int>(std::ceil(kLowHz * fft_n_ / opt_.sample_rate));
    bin_hi_ = std::min(fft_n_ / 2,
                       static_cast<int>(std::floor(kHighHz * fft_n_ / opt_.sample_rate)));

    a_re_.assign(fft_n_, 0.0f);
    a_im_.assign(fft_n_, 0.0f);
    b_re_.assign(fft_n_, 0.0f);
    b_im_.assign(fft_n_, 0.0f);
    reset();
}

void DelayEstimator::reset() {
    ref_hist_.clear();
    mic_hist_.clear();
    total_ = 0;
    recent_.clear();
    delay_ = -1;
    stats_ = Stats();
}

void DelayEstimator::set_delay(int samples) {
    delay_ = samples >= 0 ? std::min(samples, max_lag_ - 1) : -1;
}

void DelayEstimator::process(const float* mic, const float* ref, size_t n) {
    const size_t keep = static_cast<size_t>(window_ + max_lag_);
    size_t at = 0;
    while (at < n) {
        // Up to the next hop boundary, so an estimate always sees the same
        // samples whatever the caller's chunking.
        const int64_t to_hop = hop_ - total_ % hop_;
        const size_t  k      = static_cast<size_t>(std::min<int64_t>(to_hop, static_cast<int64_t>(n - at)));
        ref_hist_.insert(ref_hist_.end(), ref + at, ref + at + k);
        mic_hist_.insert(mic_hist_.end(), mic + at, mic + at + k);
        at += k;
        total_ += static_cast<int64_t>(k);

        // Trimmed in batches rather than every call, so a caller feeding 10ms
        // at a time does not move a second and a half of samples each time.
        if (ref_hist_.size() > 2 * keep) {
            const size_t drop = ref_hist_.size() - keep;
            ref_hist_.erase(ref_hist_.begin(), ref_hist_.begin() + static_cast<ptrdiff_t>(drop));
            mic_hist_.erase(mic_hist_.begin(), mic_hist_.begin() + static_cast<ptrdiff_t>(drop));
        }
        if (total_ % hop_ == 0 && ref_hist_.size() >= keep) {
            estimate();
        }
    }
}

bool DelayEstimator::active(const float* ref) const {
    const int frame = std::max(1, opt_.sample_rate / 50);
    int frames = 0, loud = 0;
    for (int f = 0; f + frame <= window_; f += frame) {
        double p = 0.0;
        for (int i = 0; i < frame; ++i) {
            p += static_cast<double>(ref[f + i]) * ref[f + i];
        }
        ++frames;
        if (p / frame > kActivePower) {
            ++loud;
        }
    }
    return frames > 0 && loud >= kActiveShare * frames;
}

void DelayEstimator::estimate() {
    // The reference window ends max_lag_ before the newest sample, so the
    // microphone after it holds that window's echo at every lag searched.
    const size_t base = ref_hist_.size() - static_cast<size_t>(window_ + max_lag_);
    const float* ref  = &ref_hist_[base];
    const float* mic  = &mic_hist_[base];
    if (!active(ref)) {
        ++stats_.skipped;
        return;
    }
    ++stats_.windows;

    std::fill(a_re_.begin(), a_re_.end(), 0.0f);
    std::fill(a_im_.begin(), a_im_.end(), 0.0f);
    std::fill(b_re_.begin(), b_re_.end(), 0.0f);
    std::fill(b_im_.begin(), b_im_.end(), 0.0f);
    std::copy(ref, ref + window_, a_re_.begin());
    std::copy(mic, mic + window_ + max_lag_, b_re_.begin());
    fft_radix2(a_re_, a_im_, fft_n_);
    fft_radix2(b_re_, b_im_, fft_n_);

    // conj(A) * B, whitened, inside the band; zero outside. Only bins
    // 0..n/2 are formed and the rest mirrored, since both inputs are real.
    const int half = fft_n_ / 2;
    for (int k = 0; k <= half; ++k) {
        float gr = 0.0f, gi = 0.0f;
        if (k >= bin_lo_ && k <= bin_hi_) {
            const float ar = a_re_[k], ai = a_im_[k];
            const float br = b_re_[k], bi = b_im_[k];
            const float cr = ar * br + ai * bi;
            const float ci = ar * bi - ai * br;
            const float m  = std::sqrt(cr * cr + ci * ci);
            if (m > 1e-20f) {
                gr = cr / m;
                gi = ci / m;
            }
        }
        a_re_[k] = gr;
        a_im_[k] = gi;
    }
    for (int k = 1; k < half; ++k) {
        a_re_[fft_n_ - k] = a_re_[k];
        a_im_[fft_n_ - k] = -a_im_[k];
    }

    // Back to time through the forward transform: the real part of
    // FFT(conj(G)) is n times the real part of IFFT(G), and only where the
    // peak is matters, not its height.
    for (int k = 0; k < fft_n_; ++k) {
        a_im_[k] = -a_im_[k];
    }
    fft_radix2(a_re_, a_im_, fft_n_);

    int peak = 0;
    for (int k = 1; k < max_lag_; ++k) {
        if (a_re_[k] > a_re_[peak]) {
            peak = k;
        }
    }
    const int width = kPeakHalfWidthMs * opt_.sample_rate / 1000;
    float other = 0.0f;
    double sum2 = 0.0;
    for (int k = 0; k < max_lag_; ++k) {
        if (std::abs(k - peak) > width && a_re_[k] > other) {
            other = a_re_[k];
        }
        sum2 += static_cast<double>(a_re_[k]) * a_re_[k];
    }
    const double rms = std::sqrt(sum2 / max_lag_);
    stats_.peak_snr   = rms > 0.0 ? a_re_[peak] / rms : 0.0;
    stats_.peak_ratio = other > 0.0f ? a_re_[peak] / other : 0.0;
    if (stats_.peak_snr < kMinPeakSnr) {
        // Played, but nothing in the microphone lines up with it: headphones,
        // a muted speaker, or someone talking far over the echo. No vote, and
        // the streak is left as it was.
        ++stats_.weak;
        stats_.last = -1;
        return;
    }
    const float least = static_cast<float>(
        std::max(static_cast<double>(kOnsetShare * a_re_[peak]), kMinOnsetSnr * rms));
    int onset = peak;
    for (int k = 0; k < peak; ++k) {
        if (a_re_[k] >= least) {
            onset = k;
            break;
        }
    }
    stats_.last = onset;

    recent_.push_back(onset);
    if (static_cast<int>(recent_.size()) > opt_.agree) {
        recent_.erase(recent_.begin());
    }
    if (static_cast<int>(recent_.size()) < opt_.agree) {
        return;
    }
    std::vector<int> sorted(recent_);
    std::sort(sorted.begin(), sorted.end());
    if (sorted.back() - sorted.front() > opt_.tolerance) {
        return;
    }
    const int agreed = sorted[sorted.size() / 2];
    if (delay_ < 0 || std::abs(agreed - delay_) > opt_.tolerance) {
        delay_ = agreed;
        ++stats_.changes;
    }
}

}  // namespace voice
}  // namespace yoai
