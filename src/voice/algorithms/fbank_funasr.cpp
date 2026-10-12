#include "voice/algorithms/fbank_funasr.hpp"

#include "voice/algorithms/fft.hpp"

#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace yoai {
namespace voice {
namespace {

// Kaldi's mel scale. There are two conventions in the wild differing in the
// constant (1127 vs 2595/log10); this is the one FunASR trained with.
inline float hz_to_mel(float f) {
    return 1127.0f * std::log(1.0f + f / 700.0f);
}

// The floor under the log. FLT_EPSILON rather than a small round number,
// because that is what the reference uses and the log of a silent band is
// otherwise -inf, which propagates through the whole encoder.
const float kEnergyFloor = 1.1920929e-07f;

}  // namespace

FunAsrFbank::FunAsrFbank(const FunAsrFbankOptions& opt)
    : opt_(opt), n_bins_(opt.n_fft / 2 + 1) {
    // Hamming, with the (frame_length - 1) denominator -- the symmetric form.
    // The periodic form differs by one sample's worth of phase and is what most
    // FFT libraries hand you by default; it is not what this model saw.
    window_.resize(opt_.frame_length);
    for (int i = 0; i < opt_.frame_length; ++i) {
        window_[i] = 0.54f - 0.46f * std::cos(2.0f * static_cast<float>(M_PI) * i /
                                              (opt_.frame_length - 1));
    }

    // Triangular filters, equally spaced on the mel scale, overlapping by half.
    // Filter m spans mel points [m, m+2) and peaks at m+1, so n_mel filters need
    // n_mel + 2 points.
    const float bin_hz = static_cast<float>(opt_.sample_rate) / opt_.n_fft;
    const float mel_lo = hz_to_mel(opt_.low_freq);
    const float mel_hi = hz_to_mel(opt_.high_freq);
    const float mel_step = (mel_hi - mel_lo) / (opt_.n_mel + 1);

    mel_weights_.assign(static_cast<size_t>(opt_.n_mel) * n_bins_, 0.0f);
    mel_begin_.assign(opt_.n_mel, n_bins_);
    mel_end_.assign(opt_.n_mel, 0);
    for (int m = 0; m < opt_.n_mel; ++m) {
        const float left   = mel_lo + m * mel_step;
        const float center = mel_lo + (m + 1) * mel_step;
        const float right  = mel_lo + (m + 2) * mel_step;
        float* row = &mel_weights_[static_cast<size_t>(m) * n_bins_];
        for (int k = 0; k < n_bins_; ++k) {
            const float mel = hz_to_mel(bin_hz * k);
            if (mel <= left || mel >= right) {
                continue;
            }
            row[k] = mel <= center ? (mel - left) / (center - left)
                                   : (right - mel) / (right - center);
            if (k < mel_begin_[m]) {
                mel_begin_[m] = k;
            }
            mel_end_[m] = k + 1;
        }
        // A filter can come out empty at the low end, where the mel steps are
        // narrower than one FFT bin. Leave begin > end so its loop runs zero
        // times rather than reading backwards.
        if (mel_end_[m] < mel_begin_[m]) {
            mel_begin_[m] = 0;
            mel_end_[m]   = 0;
        }
    }
}

int FunAsrFbank::frames_for(size_t n) const {
    if (n < static_cast<size_t>(opt_.frame_length)) {
        return 0;
    }
    // snip_edges: only whole windows, no padding at either end.
    const int t = static_cast<int>((n - opt_.frame_length) / opt_.frame_shift) + 1;
    return (t + opt_.lfr_n - 1) / opt_.lfr_n;
}

void FunAsrFbank::mel_frame(const float* samples, float* out) const {
    const int len = opt_.frame_length;

    // Remove the DC offset per frame, then pre-emphasize. Both are Kaldi
    // defaults and both are per frame, not per utterance -- doing either over
    // the whole clip gives subtly different features.
    double mean = 0.0;
    for (int i = 0; i < len; ++i) {
        mean += samples[i];
    }
    mean /= len;

    std::vector<float> frame(len);
    for (int i = 0; i < len; ++i) {
        frame[i] = samples[i] - static_cast<float>(mean);
    }
    // Backwards, so each sample is emphasized against its ORIGINAL predecessor
    // rather than an already-emphasized one. The first sample uses itself as its
    // predecessor, which is Kaldi's edge convention.
    for (int i = len - 1; i > 0; --i) {
        frame[i] -= opt_.preemphasis * frame[i - 1];
    }
    frame[0] -= opt_.preemphasis * frame[0];

    std::vector<float> re(opt_.n_fft, 0.0f);
    std::vector<float> im(opt_.n_fft, 0.0f);
    for (int i = 0; i < len; ++i) {
        re[i] = frame[i] * window_[i];
    }
    fft_radix2(re, im, opt_.n_fft);

    for (int m = 0; m < opt_.n_mel; ++m) {
        const float* row = &mel_weights_[static_cast<size_t>(m) * n_bins_];
        float energy = 0.0f;
        for (int k = mel_begin_[m]; k < mel_end_[m]; ++k) {
            energy += row[k] * (re[k] * re[k] + im[k] * im[k]);
        }
        out[m] = std::log(energy > kEnergyFloor ? energy : kEnergyFloor);
    }
}

std::vector<float> FunAsrFbank::compute(const float* pcm, size_t n, int* frames) const {
    *frames = 0;
    if (pcm == nullptr || n < static_cast<size_t>(opt_.frame_length)) {
        return std::vector<float>();
    }

    // The model was trained on int16-valued samples, not on [-1, 1] floats, and
    // the log at the end of the filterbank turns that scale difference into a
    // constant offset on every bin. The encoder is not invariant to it.
    std::vector<float> scaled(n);
    for (size_t i = 0; i < n; ++i) {
        scaled[i] = pcm[i] * 32768.0f;
    }

    const int n_frames = static_cast<int>((n - opt_.frame_length) / opt_.frame_shift) + 1;
    std::vector<float> mel(static_cast<size_t>(n_frames) * opt_.n_mel);
    for (int t = 0; t < n_frames; ++t) {
        mel_frame(&scaled[static_cast<size_t>(t) * opt_.frame_shift],
                  &mel[static_cast<size_t>(t) * opt_.n_mel]);
    }

    // Stack lfr_m frames per output row, advancing lfr_n. The first row is
    // centered on frame 0, so (lfr_m - 1) / 2 copies of frame 0 are prepended;
    // the tail repeats the last frame until the final window is full. Both ends
    // repeat rather than zero-pad, because a zero row is not silence after a
    // log -- it is an energy of exactly 1.0 in every band.
    const int pad   = (opt_.lfr_m - 1) / 2;
    const int n_out = (n_frames + opt_.lfr_n - 1) / opt_.lfr_n;
    const int need  = (n_out - 1) * opt_.lfr_n + opt_.lfr_m;

    std::vector<float> padded(static_cast<size_t>(pad + n_frames) * opt_.n_mel);
    for (int i = 0; i < pad; ++i) {
        std::memcpy(&padded[static_cast<size_t>(i) * opt_.n_mel], &mel[0],
                    opt_.n_mel * sizeof(float));
    }
    std::memcpy(&padded[static_cast<size_t>(pad) * opt_.n_mel], &mel[0],
                static_cast<size_t>(n_frames) * opt_.n_mel * sizeof(float));
    int have = pad + n_frames;
    if (have < need) {
        padded.resize(static_cast<size_t>(need) * opt_.n_mel);
        const float* last = &mel[static_cast<size_t>(n_frames - 1) * opt_.n_mel];
        for (int i = have; i < need; ++i) {
            std::memcpy(&padded[static_cast<size_t>(i) * opt_.n_mel], last,
                        opt_.n_mel * sizeof(float));
        }
        have = need;
    }

    const int d = dim();
    std::vector<float> out(static_cast<size_t>(n_out) * d);
    for (int i = 0; i < n_out; ++i) {
        for (int j = 0; j < opt_.lfr_m; ++j) {
            std::memcpy(&out[static_cast<size_t>(i) * d + static_cast<size_t>(j) * opt_.n_mel],
                        &padded[static_cast<size_t>(i * opt_.lfr_n + j) * opt_.n_mel],
                        opt_.n_mel * sizeof(float));
        }
    }

    *frames = n_out;
    return out;
}

}  // namespace voice
}  // namespace yoai
