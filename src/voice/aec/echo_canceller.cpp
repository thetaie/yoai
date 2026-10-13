#include "voice/aec/echo_canceller.hpp"

#include "voice/algorithms/fft.hpp"

#include <algorithm>
#include <cmath>

namespace yoai {
namespace voice {
namespace {

// Powers below are per sample, in the [-1, 1] scale the samples arrive in.

// A reference quieter than this, -60 dBFS, is treated as silence: nothing
// played, nothing to learn from.
const double kSilentPower = 1e-6;

// Keeps the normalization finite in a bin the reference never reaches, at
// about the level of one 16-bit step.
const double kFloorPower = 1e-9;

// Valin's leakage is kept in [kMinLeak, 1]. The floor keeps a filter that
// looks perfect still adapting a little, so a path that moves is followed.
const double kMinLeak = 0.005;

// The DC notch's pole: a first-order high-pass at about 13 Hz at 16 kHz.
const float kDcPole = 0.995f;

// Blocks in a row in which the output is louder than the input before the
// filter is judged to have diverged and is thrown away.
const int kDivergedBlocks = 50;

int next_pow2(int v) {
    int p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

}  // namespace

EchoCanceller::EchoCanceller(const EchoCancellerOptions& opt) : opt_(opt) {
    if (opt_.sample_rate <= 0) {
        opt_.sample_rate = 16000;
    }
    opt_.block = std::min(4096, std::max(16, next_pow2(opt_.block)));
    if (opt_.filter_ms <= 0) {
        opt_.filter_ms = 256;
    }
    n_        = opt_.block;
    fft_size_ = 2 * n_;
    bins_     = n_ + 1;
    const long span = static_cast<long>(opt_.filter_ms) * opt_.sample_rate / 1000;
    m_ = std::max(1, static_cast<int>((span + n_ - 1) / n_));
    opt_.filter_ms = static_cast<int>(static_cast<long>(m_) * n_ * 1000 / opt_.sample_rate);

    t_.assign(fft_size_, 0.0f);
    re_.assign(fft_size_, 0.0f);
    im_.assign(fft_size_, 0.0f);
    f_re_.assign(bins_, 0.0f);
    f_im_.assign(bins_, 0.0f);
    reset();
}

void EchoCanceller::reset() {
    mic_in_.clear();
    ref_in_.clear();
    ref_prev_.assign(n_, 0.0f);
    x_re_.assign(static_cast<size_t>(m_) * bins_, 0.0f);
    x_im_.assign(static_cast<size_t>(m_) * bins_, 0.0f);
    x_head_ = 0;
    dc_in_  = 0.0f;
    dc_out_ = 0.0f;
    clear_filter();
    stats_ = Stats();
}

void EchoCanceller::clear_filter() {
    w_re_.assign(static_cast<size_t>(m_) * bins_, 0.0f);
    w_im_.assign(static_cast<size_t>(m_) * bins_, 0.0f);
    e_mean_.assign(bins_, 0.0f);
    y_mean_.assign(bins_, 0.0f);
    // Tiny and equal: the leakage starts at 1, and the first blocks of real
    // data outweigh the start at once.
    pey_       = 1e-20;
    pyy_       = 1e-20;
    sum_adapt_ = 0.0;
    diverging_ = 0;
    stats_.adapted = false;
    stats_.leak    = 1.0;
}

void EchoCanceller::forward(const float* time, float* re, float* im) {
    std::copy(time, time + fft_size_, re_.begin());
    std::fill(im_.begin(), im_.end(), 0.0f);
    fft_radix2(re_, im_, fft_size_);
    std::copy(re_.begin(), re_.begin() + bins_, re);
    std::copy(im_.begin(), im_.begin() + bins_, im);
}

// The inverse through the forward transform: conjugate, transform, conjugate,
// divide. The input is the half spectrum of a real signal, so the conjugate of
// the full spectrum is bins 0..n conjugated, then n+1..2n-1 read back from the
// mirror unconjugated.
void EchoCanceller::inverse(const float* re, const float* im, float* time) {
    for (int k = 0; k < bins_; ++k) {
        re_[k] = re[k];
        im_[k] = -im[k];
    }
    for (int k = bins_; k < fft_size_; ++k) {
        re_[k] = re[fft_size_ - k];
        im_[k] = im[fft_size_ - k];
    }
    fft_radix2(re_, im_, fft_size_);
    const float scale = 1.0f / fft_size_;
    for (int i = 0; i < fft_size_; ++i) {
        time[i] = re_[i] * scale;
    }
}

void EchoCanceller::process(const float* mic, const float* ref, size_t n, std::vector<float>* out) {
    if (n > 0) {
        mic_in_.insert(mic_in_.end(), mic, mic + n);
        ref_in_.insert(ref_in_.end(), ref, ref + n);
    }
    size_t at = 0;
    while (mic_in_.size() - at >= static_cast<size_t>(n_)) {
        const size_t base = out->size();
        out->resize(base + n_);
        process_block(&mic_in_[at], &ref_in_[at], &(*out)[base]);
        at += n_;
    }
    if (at > 0) {
        mic_in_.erase(mic_in_.begin(), mic_in_.begin() + static_cast<ptrdiff_t>(at));
        ref_in_.erase(ref_in_.begin(), ref_in_.begin() + static_cast<ptrdiff_t>(at));
    }
}

void EchoCanceller::process_block(const float* mic, const float* ref, float* out) {
    const int N = n_;
    const int K = bins_;
    ++stats_.blocks;

    // The microphone without its DC. A capture offset is not echo and the
    // filter would otherwise spend its low bins chasing it.
    std::vector<float> d(N);
    for (int i = 0; i < N; ++i) {
        const float v = dc_out_ * kDcPole + mic[i] - dc_in_;
        dc_in_  = mic[i];
        dc_out_ = v;
        d[i]    = v;
    }

    // The reference window, the previous block then this one, becomes the
    // newest spectrum. Partition p meets the window p blocks back, which sits
    // at slot (x_head_ + p) % m_.
    std::copy(ref_prev_.begin(), ref_prev_.end(), t_.begin());
    std::copy(ref, ref + N, t_.begin() + N);
    std::copy(ref, ref + N, ref_prev_.begin());
    x_head_ = (x_head_ + m_ - 1) % m_;
    forward(&t_[0], &x_re_[static_cast<size_t>(x_head_) * K], &x_im_[static_cast<size_t>(x_head_) * K]);

    // The echo estimate: every partition's filter times the window it meets,
    // summed, back to time. Overlap-save: the second half is the linear part.
    std::fill(f_re_.begin(), f_re_.end(), 0.0f);
    std::fill(f_im_.begin(), f_im_.end(), 0.0f);
    for (int p = 0; p < m_; ++p) {
        const size_t xs = static_cast<size_t>((x_head_ + p) % m_) * K;
        const size_t ws = static_cast<size_t>(p) * K;
        for (int k = 0; k < K; ++k) {
            const float a = w_re_[ws + k], b = w_im_[ws + k];
            const float c = x_re_[xs + k], e = x_im_[xs + k];
            f_re_[k] += a * c - b * e;
            f_im_[k] += a * e + b * c;
        }
    }
    inverse(&f_re_[0], &f_im_[0], &t_[0]);
    std::vector<float> y(t_.begin() + N, t_.begin() + 2 * N);

    // The error is the output.
    std::vector<float> e(N);
    double see = 0.0, syy = 0.0, sey = 0.0, sdd = 0.0, sxx = 0.0;
    for (int i = 0; i < N; ++i) {
        e[i] = d[i] - y[i];
        see += static_cast<double>(e[i]) * e[i];
        syy += static_cast<double>(y[i]) * y[i];
        sey += static_cast<double>(e[i]) * y[i];
        sdd += static_cast<double>(d[i]) * d[i];
        sxx += static_cast<double>(ref[i]) * ref[i];
    }

    // A filter gone wrong makes the output louder than the input and keeps it
    // so. Throw it away and start again rather than add echo.
    if (!std::isfinite(see) || !std::isfinite(syy)) {
        std::copy(d.begin(), d.end(), out);
        clear_filter();
        ++stats_.resets;
        return;
    }
    std::copy(e.begin(), e.end(), out);
    if (see > sdd + N * 1e-8) {
        if (++diverging_ >= kDivergedBlocks) {
            clear_filter();
            ++stats_.resets;
            return;
        }
    } else {
        diverging_ = 0;
    }

    // Spectra of the error and of the echo estimate, each as the second half of
    // a zero-padded window: the error's is what the update uses, and both feed
    // the leakage.
    std::vector<float> e_re(K), e_im(K), yb_re(K), yb_im(K);
    std::fill(t_.begin(), t_.begin() + N, 0.0f);
    std::copy(e.begin(), e.end(), t_.begin() + N);
    forward(&t_[0], &e_re[0], &e_im[0]);
    std::copy(y.begin(), y.end(), t_.begin() + N);
    forward(&t_[0], &yb_re[0], &yb_im[0]);

    // The leakage (Valin 2007): regress the error's power spectrum on the echo
    // estimate's, each around a slow mean, and smooth the regression with a
    // rate that is fast when the echo estimate dominates the error and slow
    // when something else -- someone talking -- fills it.
    const double spec_average = static_cast<double>(N) / opt_.sample_rate;       // a 1 s mean
    const double beta0        = 2.0 * N / opt_.sample_rate;
    const double beta_max     = 0.5 * N / opt_.sample_rate;
    std::vector<float> ef(K), yf(K);
    double pey = 0.0, pyy = 0.0;
    for (int k = 0; k < K; ++k) {
        ef[k] = e_re[k] * e_re[k] + e_im[k] * e_im[k];
        yf[k] = yb_re[k] * yb_re[k] + yb_im[k] * yb_im[k];
        const double de = ef[k] - e_mean_[k];
        const double dy = yf[k] - y_mean_[k];
        pey += de * dy;
        pyy += dy * dy;
        e_mean_[k] = static_cast<float>((1.0 - spec_average) * e_mean_[k] + spec_average * ef[k]);
        y_mean_[k] = static_cast<float>((1.0 - spec_average) * y_mean_[k] + spec_average * yf[k]);
    }
    const double see_f = see + N * kFloorPower;
    const double alpha = std::min(beta0 * syy / see_f, beta_max);
    pey_ = (1.0 - alpha) * pey_ + alpha * pey;
    pyy_ = (1.0 - alpha) * pyy_ + alpha * pyy;
    pyy_ = std::max(pyy_, 1e-30);
    pey_ = std::min(std::max(pey_, kMinLeak * pyy_), pyy_);
    const double leak = pey_ / pyy_;
    stats_.leak = leak;

    // Nothing played, nothing to learn from.
    if (sxx < N * kSilentPower) {
        return;
    }

    // The step, per bin. Before the filter has adapted its echo estimate is
    // near zero and Valin's step would be too, so it bootstraps on a fixed step.
    //
    // Fixed, not scaled by the reference against the error as Speex does
    // (0.25 * min(1, Sxx / See)). That scaling reads a microphone louder than
    // the reference as someone talking, which assumes the room attenuates --
    // but how loud the echo is against the reference is the playback volume
    // and the microphone's gain, not a fact about talking. A path 12 dB up
    // took the step to 0.015 and the filter never left the bootstrap. The
    // cost of fixed: someone talking in the first second of playback is
    // learned as echo, until Valin's step takes over and unlearns it.
    std::vector<float> step(K);
    if (!stats_.adapted) {
        const double rate = 0.25;
        std::fill(step.begin(), step.end(), static_cast<float>(rate));
        sum_adapt_ += rate;
        if (sum_adapt_ > m_ && leak > 0.03) {
            stats_.adapted = true;
        }
    } else {
        // The residual-to-error ratio for the whole block, bounded below by how
        // much of the error still correlates with the estimate.
        double rer = (1e-4 * sxx + 3.0 * leak * syy) / see_f;
        rer = std::max(rer, sey * sey / (see_f * (syy + N * kFloorPower)));
        rer = std::min(rer, 0.5);
        const double floor_f = N * kFloorPower;
        for (int k = 0; k < K; ++k) {
            const double ek = ef[k] + floor_f;
            const double r  = std::min(leak * yf[k], 0.5 * ek);
            step[k] = static_cast<float>((0.7 * r + 0.3 * rer * ek) / ek);
        }
    }

    // The normalization: the reference's power in each bin over every window
    // the filter spans.
    std::vector<float> norm(K, 0.0f);
    for (int p = 0; p < m_; ++p) {
        const size_t xs = static_cast<size_t>(p) * K;
        for (int k = 0; k < K; ++k) {
            norm[k] += x_re_[xs + k] * x_re_[xs + k] + x_im_[xs + k] * x_im_[xs + k];
        }
    }
    const float delta = static_cast<float>(m_ * fft_size_ * kFloorPower);

    // Each partition moves along its window's correlation with the error, then
    // is constrained back to a causal filter of one block: overlap-save needs
    // the second half of every partition's taps to stay zero.
    for (int p = 0; p < m_; ++p) {
        const size_t xs = static_cast<size_t>((x_head_ + p) % m_) * K;
        const size_t ws = static_cast<size_t>(p) * K;
        for (int k = 0; k < K; ++k) {
            const float g  = step[k] / (norm[k] + delta);
            const float xr = x_re_[xs + k], xi = x_im_[xs + k];
            // conj(X) * E
            const float cr = xr * e_re[k] + xi * e_im[k];
            const float ci = xr * e_im[k] - xi * e_re[k];
            w_re_[ws + k] += g * cr;
            w_im_[ws + k] += g * ci;
        }
        inverse(&w_re_[ws], &w_im_[ws], &t_[0]);
        std::fill(t_.begin() + N, t_.end(), 0.0f);
        forward(&t_[0], &w_re_[ws], &w_im_[ws]);
    }
}

}  // namespace voice
}  // namespace yoai
