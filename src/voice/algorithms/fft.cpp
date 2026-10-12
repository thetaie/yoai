#include "voice/algorithms/fft.hpp"

#include <cmath>
#include <utility>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace yoai {
namespace voice {

// The bit-reversal permutation and the butterflies need no special cases for
// a power of two.
void fft_radix2(std::vector<float>& re, std::vector<float>& im, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / len;
        const float  wr  = static_cast<float>(std::cos(ang));
        const float  wi  = static_cast<float>(std::sin(ang));
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f;
            float ci = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                const float ur = re[i + k];
                const float ui = im[i + k];
                const float xr = re[i + k + len / 2];
                const float xi = im[i + k + len / 2];
                const float vr = xr * cr - xi * ci;
                const float vi = xr * ci + xi * cr;
                re[i + k]           = ur + vr;
                im[i + k]           = ui + vi;
                re[i + k + len / 2] = ur - vr;
                im[i + k + len / 2] = ui - vi;
                const float nc = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nc;
            }
        }
    }
}

}  // namespace voice
}  // namespace yoai
