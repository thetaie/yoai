// The echo canceller's C ABI, against the class it wraps.
//
// echo_canceller_test.cpp pins what the canceller does. This pins the plumbing
// a host goes through, which can be wrong while the canceller is right: the
// reference brought to 16 kHz with the resampler's lead dropped, the two
// streams paired however they arrive, output kept back when it does not fit,
// the samples converted, and the settings applied when yoai.h says they are.
//
// Each run compares yoai_aec_process() with EchoCanceller run directly on the
// same signals, and asks for the same bits.
//
//     build/yoai-aec-abi-test

#include "yoai.h"

#include "voice/aec/echo_canceller.hpp"
#include "voice/algorithms/resampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using yoai::voice::EchoCanceller;
using yoai::voice::EchoCancellerOptions;
using yoai::voice::Resampler;

namespace {

const int kRate    = 16000;
const int kRefRate = 24000;

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

// Deterministic noise, so every run sees the same signals.
struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    double uniform() {   // [0, 1)
        s = s * 1664525u + 1013904223u;
        return (s >> 8) / 16777216.0;
    }
    double gauss() {
        const double u = std::max(uniform(), 1e-12);
        const double v = uniform();
        return std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * 3.14159265358979323846 * v);
    }
    size_t below(size_t n) { return static_cast<size_t>(uniform() * n); }
};

std::vector<float> white(size_t n, double amplitude, uint32_t seed) {
    Lcg rng(seed);
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) {
        x[i] = static_cast<float>(amplitude * (2.0 * rng.uniform() - 1.0));
    }
    return x;
}

// The room of echo_canceller_test.cpp: `delay_ms` of buffering, a direct path,
// and a tail decaying 60 dB over `rt60_ms`, cut at `len_ms`.
std::vector<float> room(double delay_ms, double rt60_ms, double len_ms, uint32_t seed) {
    Lcg rng(seed);
    const int delay = static_cast<int>(delay_ms * kRate / 1000);
    const int len   = static_cast<int>(len_ms * kRate / 1000);
    std::vector<float> h(static_cast<size_t>(delay + len), 0.0f);
    h[delay] = 1.0f;
    const double decay = 6.9078 / (rt60_ms * kRate / 1000);
    for (int i = 1; i < len; ++i) {
        h[delay + i] = static_cast<float>(0.3 * rng.gauss() * std::exp(-decay * i));
    }
    return h;
}

std::vector<float> convolve(const std::vector<float>& x, const std::vector<float>& h) {
    std::vector<float> y(x.size(), 0.0f);
    for (size_t i = 0; i < x.size(); ++i) {
        double acc = 0.0;
        for (size_t j = 0; j < h.size() && j <= i; ++j) {
            if (h[j] != 0.0f) {
                acc += static_cast<double>(h[j]) * x[i - j];
            }
        }
        y[i] = static_cast<float>(acc);
    }
    return y;
}

double power(const std::vector<float>& x, size_t from, size_t to) {
    double s = 0.0;
    to = std::min(to, x.size());
    for (size_t i = from; i < to; ++i) {
        s += static_cast<double>(x[i]) * x[i];
    }
    return to > from ? s / (to - from) : 0.0;
}

// yoai.h's conversion both ways: x / 32768 in, round to nearest and clamp out.
short to_pcm16(float x) {
    const float v = x * 32768.0f;
    if (v >= 32767.0f) {
        return 32767;
    }
    if (v <= -32768.0f) {
        return -32768;
    }
    return static_cast<short>(std::lrint(v));
}

// The signals every run uses: a reference at 24 kHz, the same at 16 kHz the way
// the ABI makes it, and a microphone hearing it through a room.
struct Signals {
    std::vector<float> ref24;
    std::vector<float> ref16;
    std::vector<float> echo;
    std::vector<short> mic;
    int lead = 0;   // the resampler's, in samples at 16 kHz
};

Signals make_signals() {
    Signals s;
    const size_t n16 = 6 * kRate;
    // Quiet enough that the echo, with the room's tail summed in, stays inside
    // 16 bits: at 0.3 the microphone clipped, which no linear filter undoes, and
    // the cancelling check read 17 dB.
    s.ref24 = white(6 * kRefRate, 0.05, 1);

    // In one piece: the resampler's output is the same however its input is
    // split, which resampler_test.cpp checks, so this is what the ABI's
    // streaming conversion has to come to.
    Resampler r(kRefRate, kRate);
    std::vector<float> y;
    r.process(&s.ref24[0], s.ref24.size(), &y);
    s.lead = r.delay();
    s.ref16.assign(y.begin() + s.lead, y.end());

    std::vector<float> ref16_full(s.ref16);
    ref16_full.resize(n16, 0.0f);
    s.echo = convolve(ref16_full, room(20, 150, 125, 2));
    const std::vector<float> noise = white(n16, 1e-3, 3);
    s.mic.resize(n16);
    for (size_t i = 0; i < n16; ++i) {
        s.mic[i] = to_pcm16(s.echo[i] + noise[i]);
    }
    return s;
}

// The class, on what the ABI is given, paired as the ABI pairs it.
std::vector<short> direct(const Signals& s, int filter_ms) {
    EchoCancellerOptions opt;
    opt.sample_rate = kRate;
    opt.filter_ms   = filter_ms;
    EchoCanceller aec(opt);
    const size_t pairs = std::min(s.mic.size(), s.ref16.size());
    std::vector<float> mic(pairs);
    for (size_t i = 0; i < pairs; ++i) {
        mic[i] = s.mic[i] / 32768.0f;
    }
    std::vector<float> out;
    aec.process(&mic[0], &s.ref16[0], pairs, &out);
    std::vector<short> pcm(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        pcm[i] = to_pcm16(out[i]);
    }
    return pcm;
}

// Everything in one call, with room for all of it.
std::vector<short> whole(const std::vector<short>& mic, const std::vector<float>& ref) {
    std::vector<short> out(mic.size() + 512);
    const int n = yoai_aec_process(&mic[0], static_cast<int>(mic.size()), &ref[0],
                                   static_cast<int>(ref.size()), &out[0],
                                   static_cast<int>(out.size()));
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

void same(const std::vector<short>& got, const std::vector<short>& want, const char* what) {
    const bool equal = got.size() == want.size() &&
                       (got.empty() || std::memcmp(&got[0], &want[0], got.size() * sizeof(short)) == 0);
    char line[200];
    std::snprintf(line, sizeof(line), "%s: %zu samples, %s", what, got.size(),
                  equal ? "the same bits" : "DIFFERENT");
    check(equal, line);
    if (!equal) {
        size_t i = 0;
        while (i < got.size() && i < want.size() && got[i] == want[i]) {
            ++i;
        }
        std::printf("      expected %zu samples; first difference at %zu\n", want.size(), i);
    }
}

//------------------------------------------------------------------------------
// Checks
//------------------------------------------------------------------------------

void test_streamed(const Signals& s, const std::vector<short>& want) {
    std::printf("the two streams in uneven pieces, one often ahead, into small buffers\n");
    yoai_aec_set_high_latency(0);
    yoai_aec_set_reference_rate(kRefRate);
    yoai_aec_reset();

    Lcg rng(9);
    std::vector<short> got;
    std::vector<short> buf(1200);
    size_t im = 0, ir = 0;
    int calls = 0, held = 0;
    while (im < s.mic.size() || ir < s.ref24.size()) {
        // A quarter of the calls bring only the microphone, a quarter only the
        // reference, so either can run well ahead of the other.
        const size_t pick = rng.below(4);
        size_t nm = pick == 1 ? 0 : std::min(rng.below(700), s.mic.size() - im);
        size_t nr = pick == 2 ? 0 : std::min(rng.below(1100), s.ref24.size() - ir);
        const int cap = static_cast<int>(rng.below(600));
        const int n = yoai_aec_process(nm ? &s.mic[im] : nullptr, static_cast<int>(nm),
                                       nr ? &s.ref24[ir] : nullptr, static_cast<int>(nr),
                                       cap ? &buf[0] : nullptr, cap);
        if (n < 0) {
            std::printf("FAIL  process: %s\n", yoai_aec_last_error());
            ++g_failures;
            return;
        }
        if (n == cap && cap > 0) {
            ++held;
        }
        got.insert(got.end(), buf.begin(), buf.begin() + n);
        im += nm;
        ir += nr;
        ++calls;
    }
    // Whatever did not fit, drained by calls with no input.
    for (;;) {
        const int n = yoai_aec_process(nullptr, 0, nullptr, 0, &buf[0], 300);
        if (n <= 0) {
            break;
        }
        got.insert(got.end(), buf.begin(), buf.begin() + n);
    }
    std::printf("      %d calls, %d of them with a full buffer\n", calls, held);
    same(got, want, "streamed against the class");

    // That it cancels at all, through this door: the last two seconds.
    const size_t a = s.mic.size() - 2 * kRate;
    std::vector<float> out(got.size());
    for (size_t i = 0; i < got.size(); ++i) {
        out[i] = got[i] / 32768.0f;
    }
    const double erle = 10.0 * std::log10(power(s.echo, a, got.size()) / power(out, a, got.size()));
    char line[120];
    std::snprintf(line, sizeof(line), "it cancels: ERLE %.1f dB over the last two seconds", erle);
    check(erle > 20.0, line);
}

void test_high_latency(const Signals& s, const std::vector<short>& want256,
                       const std::vector<short>& want512) {
    std::printf("high latency\n");
    yoai_aec_set_high_latency(1);
    yoai_aec_reset();
    same(whole(s.mic, s.ref24), want512, "512ms after a reset");

    // A setting waits for the canceller to be made again.
    yoai_aec_set_high_latency(0);
    yoai_aec_reset();
    yoai_aec_set_high_latency(1);
    same(whole(s.mic, s.ref24), want256, "set without a reset: still 256ms");
    yoai_aec_set_high_latency(0);
}

void test_reference_rate(const Signals& s, const std::vector<short>& want) {
    std::printf("reference rate\n");
    check(yoai_aec_set_reference_rate(kRate) == 0 && yoai_aec_last_error()[0] == '\0',
          "16000 accepted");
    yoai_aec_reset();
    same(whole(s.mic, s.ref16), want, "a 16 kHz reference, passed through");

    check(yoai_aec_set_reference_rate(44100) == 0, "44100 accepted");
    check(yoai_aec_set_reference_rate(0) == -1, "0 refused");
    std::printf("      %s\n", yoai_aec_last_error());
    check(yoai_aec_set_reference_rate(7) == -1 && yoai_aec_last_error()[0] != '\0',
          "7 refused: a ratio the resampler cannot take");
    check(yoai_aec_set_reference_rate(kRefRate) == 0, "back to 24000");
    yoai_aec_reset();
}

void test_bad_calls() {
    std::printf("bad calls\n");
    short buf[16];
    const float ref[4] = {0, 0, 0, 0};
    check(yoai_aec_process(nullptr, 10, ref, 4, buf, 16) == -1 && yoai_aec_last_error()[0] != '\0',
          "a NULL microphone with samples is refused, with a reason");
    std::printf("      %s\n", yoai_aec_last_error());
    check(yoai_aec_process(nullptr, 0, ref, -1, buf, 16) == -1, "a negative count is refused");
    check(yoai_aec_process(nullptr, 0, ref, 4, nullptr, 16) == -1, "a NULL output with room is refused");
    check(yoai_aec_process(nullptr, 0, ref, 4, buf, 16) >= 0 && yoai_aec_last_error()[0] == '\0',
          "the next good call clears the reason");
    yoai_aec_reset();
}

}  // namespace

int main() {
    const Signals s = make_signals();
    std::printf("reference %zu samples at 24 kHz, %zu at 16 kHz after a %d-sample lead (%.1f ms); "
                "microphone %zu\n\n",
                s.ref24.size(), s.ref16.size(), s.lead, s.lead * 1000.0 / kRate, s.mic.size());
    const std::vector<short> want256 = direct(s, 256);
    const std::vector<short> want512 = direct(s, 512);

    test_streamed(s, want256);
    test_high_latency(s, want256, want512);
    test_reference_rate(s, want256);
    test_bad_calls();

    std::printf("\n%s\n", g_failures == 0 ? "all aec abi checks passed" : "SOME AEC ABI CHECKS FAILED");
    return g_failures == 0 ? 0 : 1;
}
