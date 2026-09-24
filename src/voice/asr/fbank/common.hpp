// The parts of a filterbank front end that are not a model's convention.
//
// Deliberately small. Front ends differ in nearly every step -- the window, the
// mel scale, how the log is floored, how frames are stacked -- and each of
// those is part of what a model was trained on, so it belongs in that model's
// file (funasr.cpp) rather than here. What is left is arithmetic with one right
// answer.
//
// **Changing anything here changes every front end that uses it, in the last
// bits**, and funasr.hpp explains why that matters: bit-level agreement with
// the reference implementation is how that front end is known to be right.

#pragma once

#include <vector>

namespace yoai {
namespace voice {
namespace fbank {

// In-place radix-2 FFT. `n` must be a power of two and both vectors at least
// `n` long; for real input, pass `im` zeroed.
//
// Complex input rather than a real-input transform, which would halve the
// work: at 512 points per 10 ms this is far under the encoder's cost, and the
// simple form is the one that can be read against the reference.
void fft_radix2(std::vector<float>& re, std::vector<float>& im, int n);

}  // namespace fbank
}  // namespace voice
}  // namespace yoai
