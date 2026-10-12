// The FFT every signal-processing file in voice/ shares.
//
// **Changing anything here changes every front end that uses it, in the last
// bits**, and fbank_funasr.hpp explains why that matters: bit-level agreement
// with FunASR's front end is how that front end is known to be right. A user
// that wants something else from an FFT -- a real-input transform, another
// twiddle schedule -- gets its own function beside this one, not a change to
// this one.
//
// There is no inverse. Conjugate, transform, conjugate and divide by n: the
// same butterflies, so the same last bits.

#pragma once

#include <vector>

namespace yoai {
namespace voice {

// In-place radix-2 FFT. `n` must be a power of two and both vectors at least
// `n` long; for real input, pass `im` zeroed.
//
// Complex input rather than a real-input transform, which would halve the
// work: at 512 points per 10 ms this is far under the encoder's cost, and the
// simple form is the one that can be read against the reference.
void fft_radix2(std::vector<float>& re, std::vector<float>& im, int n);

}  // namespace voice
}  // namespace yoai
