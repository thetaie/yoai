#include "voice/algorithms/resampler.hpp"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace yoai {
namespace voice {
namespace {

// The filter's two specifications; see the header for why these values.
const double kStopbandDb = 80.0;
const double kPassband   = 0.90;   // of the lower rate's Nyquist

int gcd(int a, int b) {
    while (b != 0) {
        const int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// The modified Bessel function of the first kind, order zero, summed from its
// power series. The Kaiser window is a ratio of two of these, and at the beta
// used here the series is done within about thirty terms.
double bessel_i0(double x) {
    const double q = x * x / 4.0;
    double sum  = 1.0;
    double term = 1.0;
    for (int k = 1; k < 500; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

}  // namespace

Resampler::Resampler(int in_rate, int out_rate) : in_rate_(in_rate), out_rate_(out_rate) {
    if (in_rate <= 0 || out_rate <= 0) {
        return;
    }
    const int g    = gcd(in_rate, out_rate);
    const int up   = out_rate / g;
    const int down = in_rate / g;
    if (up > 1024 || down > 1024) {
        return;
    }

    // Designed on the upsampled grid, in cycles per grid sample.
    const double grid    = static_cast<double>(in_rate) * up;
    const double nyquist = 0.5 * std::min(in_rate, out_rate);
    const double pass    = kPassband * nyquist / grid;
    const double stop    = nyquist / grid;
    const double cutoff  = 0.5 * (pass + stop);

    // Kaiser's two estimates: the length that reaches the attenuation across
    // the transition band, and the window's shape for that attenuation.
    const double length = (kStopbandDb - 7.95) / (14.36 * (stop - pass));
    const double beta   = 0.1102 * (kStopbandDb - 8.7);

    // The half-length is rounded up to a multiple of `down`, which makes the
    // group delay -- `half` grid samples -- a whole number of output samples.
    int half = static_cast<int>(std::ceil(length / 2.0));
    half = (half + down - 1) / down * down;
    const int total = 2 * half + 1;

    // Padded with zeros to a whole number of taps per phase.
    const int taps = (total + up - 1) / up;
    std::vector<double> h(static_cast<size_t>(taps) * up, 0.0);
    const double i0_beta = bessel_i0(beta);
    for (int j = 0; j < total; ++j) {
        const double x    = j - half;
        const double sinc = x == 0.0 ? 2.0 * cutoff
                                     : std::sin(2.0 * M_PI * cutoff * x) / (M_PI * x);
        const double r    = x / half;
        const double w    = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0_beta;
        h[static_cast<size_t>(j)] = sinc * w;
    }

    // Each phase is scaled to sum to exactly one. A constant then comes out as
    // the same constant whichever phase produced the sample, and the gain of
    // `up` that the zero-stuffing costs is restored along the way: before this,
    // a phase sums to about 1 / up.
    phases_.assign(h.size(), 0.0f);
    for (int p = 0; p < up; ++p) {
        double sum = 0.0;
        for (int k = 0; k < taps; ++k) {
            sum += h[static_cast<size_t>(p + k * up)];
        }
        for (int k = 0; k < taps; ++k) {
            phases_[static_cast<size_t>(p) * taps + k] =
                static_cast<float>(h[static_cast<size_t>(p + k * up)] / sum);
        }
    }

    up_    = up;
    down_  = down;
    taps_  = taps;
    delay_ = half / down;
    reset();
}

void Resampler::reset() {
    if (!valid()) {
        return;
    }
    buf_.assign(static_cast<size_t>(taps_ - 1), 0.0f);
    base_ = -(taps_ - 1);
    next_ = 0;
}

void Resampler::process(const float* in, size_t n, std::vector<float>* out) {
    if (!valid()) {
        return;
    }
    if (n > 0) {
        buf_.insert(buf_.end(), in, in + n);
    }
    const int64_t last = base_ + static_cast<int64_t>(buf_.size()) - 1;

    // Output m sits at m * down on the grid. The newest input it reaches is the
    // one at or before it, and which phase of the filter meets that input is how
    // far past it the output falls.
    for (;;) {
        const int64_t newest = next_ / up_;
        if (newest > last) {
            break;
        }
        const int    p = static_cast<int>(next_ - newest * up_);
        const float* h = &phases_[static_cast<size_t>(p) * taps_];
        const float* x = &buf_[static_cast<size_t>(newest - base_)];
        float acc = 0.0f;
        for (int k = 0; k < taps_; ++k) {
            acc += h[k] * x[-k];
        }
        out->push_back(acc);
        next_ += down_;
    }

    // Keep only what the next output reaches back to. When `down` exceeds `up`
    // that can start past what has arrived; drop what is here and let base_
    // stay at or before the first sample the next output needs.
    const int64_t keep_from = next_ / up_ - (taps_ - 1);
    int64_t drop = keep_from - base_;
    if (drop > static_cast<int64_t>(buf_.size())) {
        drop = static_cast<int64_t>(buf_.size());
    }
    if (drop > 0) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(drop));
        base_ += drop;
    }
}

}  // namespace voice
}  // namespace yoai
