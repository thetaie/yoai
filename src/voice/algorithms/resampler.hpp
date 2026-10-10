// A fixed-ratio resampler: polyphase FIR, Kaiser-windowed sinc.
//
// **It is for the echo canceller's reference signal, and that is the only
// reason the core has one.** The core still resamples nothing that is
// analyzed: recognition and prosody take 16 kHz from the capture and refuse
// anything else, because a resampler that differs from the one a measurement
// was taken with changes the answer quietly. The reference is
// different. Qwen3-TTS speaks at 24 kHz and the microphone is opened at 16 kHz,
// so what was played has to be brought to the capture's rate before it can be
// subtracted -- and an error here becomes a little residual echo, never a
// change to what the speaker said.
//
// The ratio is reduced to up / down (24 kHz to 16 kHz is 2 / 3): conceptually
// insert up - 1 zeros between samples, low-pass, keep every down-th. Only the
// taps that meet a real sample are ever multiplied, so each output costs one
// phase of the filter -- 154 multiply-adds at 24 to 16.
//
// The filter passes 90% of the lower rate's Nyquist and stops at Nyquist itself,
// 80 dB down: 0 to 7.2 kHz passes and nothing from above 8 kHz folds back into
// the output. Those are the usual choices for a quality resampler (soxr's
// default passband is 91%), not tuned against anything here.
//
// Streaming: process() takes any number of samples, including zero, and the
// output is the same bits however the input is split. The output lags the input
// by delay() output samples; a caller aligning two streams subtracts it.
//
// Not thread-safe; one writer.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace yoai {
namespace voice {

class Resampler {
public:
    // Any two positive rates whose reduced ratio has both terms at most 1024.
    // Anything else leaves the resampler invalid, and process() then emits
    // nothing.
    Resampler(int in_rate, int out_rate);

    bool valid() const { return up_ > 0; }
    int  in_rate() const { return in_rate_; }
    int  out_rate() const { return out_rate_; }

    // The filter's group delay, in output samples. Always a whole number: the
    // filter's half-length is rounded up to make it one, so a caller trimming
    // the lead never has to shift by a fraction.
    int delay() const { return delay_; }

    // Append the output for `n` more input samples to `out`.
    void process(const float* in, size_t n, std::vector<float>* out);

    // Back to the state of a fresh instance: silence before the next sample.
    void reset();

private:
    int in_rate_  = 0;
    int out_rate_ = 0;
    int up_       = 0;
    int down_     = 0;
    int taps_     = 0;      // per phase
    int delay_    = 0;

    // Phase-major: phase p's taps are phases_[p * taps_ .. p * taps_ + taps_),
    // tap k meeting the input sample k steps back from the newest.
    std::vector<float> phases_;

    // The input still needed, oldest first. buf_[0] is input sample `base_`,
    // where input 0 is the first sample ever passed in and the taps_ - 1 before
    // it are silence.
    std::vector<float> buf_;
    int64_t base_ = 0;

    // The next output's position on the upsampled grid, where input i sits at
    // i * up_. Output m is at m * down_.
    int64_t next_ = 0;
};

}  // namespace voice
}  // namespace yoai
