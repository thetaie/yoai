// The delay estimator in voice/algorithms, against delays this file chose.
//
// With no arguments: noise through synthetic rooms at delays from 0 to 480ms,
// the same with the reference in bursts and coloured like speech, someone
// talking over it, a delay that changes, no echo at all, a silent reference,
// and the result against its own streaming.
//
//     build/yoai-delay-estimator-test
//
// With two WAVs, both 16kHz mono: what was played and what the microphone
// heard. Prints every window's own lag and what was accepted; with --true-ms,
// whether each window got it.
//
//     build/yoai-delay-estimator-test <ref.wav> <mic.wav> [--true-ms D]

#include "voice/algorithms/delay_estimator.hpp"

#include "wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using yoai::voice::DelayEstimator;
using yoai::voice::DelayEstimatorOptions;

namespace {

const int    kRate = 16000;
const double kPi   = 3.14159265358979323846;

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    double uniform() {
        s = s * 1664525u + 1013904223u;
        return (s >> 8) / 16777216.0;
    }
    double gauss() {
        const double u = std::max(uniform(), 1e-12);
        const double v = uniform();
        return std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * kPi * v);
    }
};

std::vector<float> white(size_t n, double amplitude, uint32_t seed) {
    Lcg rng(seed);
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) {
        x[i] = static_cast<float>(amplitude * (2.0 * rng.uniform() - 1.0));
    }
    return x;
}

// Speech-like in the two ways that matter here: most of its energy is low, and
// it stops. One-pole low-pass, then 150ms on and 100ms off.
std::vector<float> bursts(size_t n, double amplitude, uint32_t seed) {
    std::vector<float> x = white(n, amplitude, seed);
    float y = 0.0f;
    const size_t on = 150 * kRate / 1000, period = 250 * kRate / 1000;
    for (size_t i = 0; i < n; ++i) {
        y = 0.9f * y + 0.1f * x[i];
        x[i] = (i % period) < on ? 3.0f * y : 0.0f;
    }
    return x;
}

// The same room as tests/echo_canceller_test.cpp: nothing for `delay_ms`, a
// direct path, then a reverberant tail decaying 60dB over `rt60_ms`.
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
    std::vector<size_t> nz;
    for (size_t j = 0; j < h.size(); ++j) {
        if (h[j] != 0.0f) {
            nz.push_back(j);
        }
    }
    std::vector<float> y(x.size(), 0.0f);
    for (size_t i = 0; i < x.size(); ++i) {
        double acc = 0.0;
        for (size_t t = 0; t < nz.size() && nz[t] <= i; ++t) {
            acc += static_cast<double>(h[nz[t]]) * x[i - nz[t]];
        }
        y[i] = static_cast<float>(acc);
    }
    return y;
}

std::vector<float> add(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> y(a);
    for (size_t i = 0; i < y.size() && i < b.size(); ++i) {
        y[i] += b[i];
    }
    return y;
}

// Fed 10ms at a time, as a capture callback would, recording when the delay
// was first accepted.
struct Run {
    int    delay;
    double first_at;   // seconds of audio when it was first accepted, -1 never
};

Run feed(DelayEstimator& d, const std::vector<float>& mic, const std::vector<float>& ref) {
    Run r = {-1, -1.0};
    const size_t step = kRate / 100;
    for (size_t at = 0; at < mic.size(); at += step) {
        const size_t len = std::min(step, mic.size() - at);
        d.process(&mic[at], &ref[at], len);
        if (r.first_at < 0 && d.has_delay()) {
            r.first_at = static_cast<double>(at + len) / kRate;
        }
    }
    r.delay = d.delay();
    return r;
}

//------------------------------------------------------------------------------

void test_delays() {
    std::printf("white noise through a room, 6s each\n");
    const double delays[] = {0, 30, 150, 300, 480};
    const std::vector<float> ref = white(6 * kRate, 0.3, 1);
    for (size_t i = 0; i < sizeof(delays) / sizeof(delays[0]); ++i) {
        const std::vector<float> mic =
            add(convolve(ref, room(delays[i], 250, 125, 2)), white(ref.size(), 1e-3, 3));
        DelayEstimator d;
        const Run r = feed(d, mic, ref);
        const int want = static_cast<int>(delays[i] * kRate / 1000);
        char what[160];
        std::snprintf(what, sizeof(what), "%3.0fms: %d samples, want %d; accepted at %.2fs, peak %.1fx",
                      delays[i], r.delay, want, r.first_at, d.stats().peak_ratio);
        check(r.delay == want, what);
    }
}

void test_speech_like() {
    std::printf("the reference coloured and in bursts, 120ms\n");
    const std::vector<float> ref = bursts(8 * kRate, 0.3, 4);
    const std::vector<float> mic =
        add(convolve(ref, room(120, 250, 125, 5)), white(ref.size(), 1e-3, 6));
    DelayEstimator d;
    const Run r = feed(d, mic, ref);
    char what[160];
    std::snprintf(what, sizeof(what), "%d samples, want 1920; accepted at %.2fs; %ld windows, %ld skipped",
                  r.delay, r.first_at, d.stats().windows, d.stats().skipped);
    check(r.delay == 1920, what);
}

void test_double_talk() {
    std::printf("someone talking throughout, 6dB over the echo\n");
    const std::vector<float> ref  = bursts(8 * kRate, 0.3, 7);
    const std::vector<float> echo = convolve(ref, room(200, 250, 125, 8));
    double pe = 0.0;
    for (size_t i = 0; i < echo.size(); ++i) {
        pe += static_cast<double>(echo[i]) * echo[i];
    }
    const double rms = std::sqrt(pe / echo.size());
    // Uniform noise of amplitude a has rms a / sqrt(3); 6dB is twice the rms.
    const std::vector<float> near = bursts(echo.size(), 2.0 * rms * std::sqrt(3.0) / 1.2, 9);
    DelayEstimator d;
    const Run r = feed(d, add(echo, near), ref);
    char what[160];
    std::snprintf(what, sizeof(what), "%d samples, want 3200; accepted at %.2fs", r.delay, r.first_at);
    check(r.delay == 3200, what);
}

void test_change() {
    std::printf("40ms for 8s, then 90ms\n");
    const size_t n = 16 * kRate, cut = 8 * kRate;
    const std::vector<float> ref = white(n, 0.3, 10);
    const std::vector<float> a   = convolve(ref, room(40, 250, 125, 11));
    const std::vector<float> b   = convolve(ref, room(90, 250, 125, 12));
    std::vector<float> mic(n);
    for (size_t i = 0; i < n; ++i) {
        mic[i] = i < cut ? a[i] : b[i];
    }
    DelayEstimator d;
    const std::vector<float> m0(mic.begin(), mic.begin() + cut), r0(ref.begin(), ref.begin() + cut);
    feed(d, m0, r0);
    const int before = d.delay();
    const std::vector<float> m1(mic.begin() + cut, mic.end()), r1(ref.begin() + cut, ref.end());
    const Run after = feed(d, m1, r1);
    char what[160];
    std::snprintf(what, sizeof(what), "640 before, %d; 1440 after, %d; %d changes", before,
                  after.delay, d.stats().changes);
    check(before == 640 && after.delay == 1440 && d.stats().changes == 2, what);
}

// The direct path partly blocked: a reflection 8ms behind it and stronger. The
// answer may be either -- the canceller keeps a margin ahead of whatever it is
// told -- but it has to settle on one.
void test_strong_reflection() {
    std::printf("a reflection 8ms behind the direct path and louder than it, 60ms\n");
    const std::vector<float> ref = bursts(10 * kRate, 0.3, 21);
    std::vector<float> h = room(60, 250, 125, 22);
    h[960] = 0.6f;
    h[960 + 128] += 1.0f;
    const std::vector<float> mic = add(convolve(ref, h), white(ref.size(), 1e-3, 23));
    DelayEstimator d;
    const Run r = feed(d, mic, ref);
    char what[160];
    std::snprintf(what, sizeof(what), "%d samples, want 960 or 1088; %d change(s)", r.delay,
                  d.stats().changes);
    check((r.delay == 960 || r.delay == 1088) && d.stats().changes == 1, what);
}

void test_no_echo() {
    std::printf("no echo: the microphone hears something else, 120s\n");
    const size_t n = 120 * kRate;
    const std::vector<float> ref = bursts(n, 0.3, 13);
    const std::vector<float> mic = add(bursts(n, 0.3, 14), white(n, 1e-3, 15));
    DelayEstimator d;
    const Run r = feed(d, mic, ref);
    char what[160];
    std::snprintf(what, sizeof(what), "nothing accepted over %ld windows, %ld without a peak (got %d)",
                  d.stats().windows, d.stats().weak, r.delay);
    check(!d.has_delay(), what);
}

void test_silence() {
    std::printf("nothing played\n");
    const std::vector<float> ref(5 * kRate, 0.0f);
    const std::vector<float> mic = white(ref.size(), 0.1, 16);
    DelayEstimator d;
    feed(d, mic, ref);
    char what[160];
    std::snprintf(what, sizeof(what), "every window skipped (%ld skipped, %ld estimated), no delay",
                  d.stats().skipped, d.stats().windows);
    check(d.stats().windows == 0 && d.stats().skipped > 0 && !d.has_delay(), what);
}

void test_prior() {
    std::printf("a prior\n");
    DelayEstimator d;
    d.set_delay(800);
    check(d.delay() == 800 && d.stats().changes == 0, "set_delay is the answer until windows agree on another");
    d.reset();
    check(!d.has_delay(), "reset forgets it");
}

void test_streaming() {
    std::printf("streaming\n");
    const std::vector<float> ref = bursts(6 * kRate, 0.3, 17);
    const std::vector<float> mic = add(convolve(ref, room(70, 250, 125, 18)), white(ref.size(), 1e-3, 19));

    DelayEstimator whole;
    whole.process(&mic[0], &ref[0], mic.size());

    DelayEstimator split;
    Lcg rng(20);
    size_t at = 0;
    while (at < mic.size()) {
        const size_t len = std::min(static_cast<size_t>(rng.uniform() * 3000), mic.size() - at);
        split.process(len ? &mic[at] : nullptr, len ? &ref[at] : nullptr, len);
        at += len;
    }
    const DelayEstimator::Stats& a = whole.stats();
    const DelayEstimator::Stats& b = split.stats();
    const bool same = whole.delay() == split.delay() && a.windows == b.windows &&
                      a.skipped == b.skipped && a.last == b.last && a.changes == b.changes &&
                      std::memcmp(&a.peak_ratio, &b.peak_ratio, sizeof(double)) == 0;
    check(same, "the same windows and the same answer however the input is split");
}

//------------------------------------------------------------------------------

int trace(const char* ref_path, const char* mic_path, double true_ms) {
    std::vector<float> ref, mic;
    int rr = 0, rm = 0;
    if (!yoai::testing::read_wav_mono(ref_path, &ref, &rr) ||
        !yoai::testing::read_wav_mono(mic_path, &mic, &rm)) {
        return 1;
    }
    if (rr != kRate || rm != kRate) {
        std::printf("both files must be 16000 Hz (got %d and %d)\n", rr, rm);
        return 1;
    }
    const size_t n = std::min(ref.size(), mic.size());
    DelayEstimator d;
    const int want = true_ms >= 0 ? static_cast<int>(std::floor(true_ms * kRate / 1000 + 0.5)) : -1;
    // Fed a hop at a time, so each call estimates at most one window.
    const size_t hop = static_cast<size_t>(d.options().hop_ms) * kRate / 1000;
    std::printf("  window at   own lag              ratio  snr    accepted\n");
    long windows = 0, skipped = 0, weak = 0;
    int right = 0, wrong = 0;
    for (size_t at = 0; at < n; at += hop) {
        const size_t len = std::min(hop, n - at);
        d.process(&mic[at], &ref[at], len);
        const DelayEstimator::Stats& s = d.stats();
        // The window just estimated ends max_lag before the newest sample.
        const double start =
            static_cast<double>(static_cast<long>(at + len) - d.window() - d.max_lag()) / kRate;
        if (s.skipped != skipped) {
            skipped = s.skipped;
            std::printf("  %6.2fs     (too little played)\n", start);
            continue;
        }
        if (s.windows == windows) {
            continue;
        }
        windows = s.windows;
        if (s.weak != weak) {
            weak = s.weak;
            std::printf("  %6.2fs     (no peak stands out)   %4.2fx  %5.1f\n", start, s.peak_ratio,
                        s.peak_snr);
            continue;
        }
        const char* mark = "";
        if (want >= 0) {
            const bool ok = std::abs(s.last - want) <= 1;
            if (ok) {
                ++right;
            } else {
                ++wrong;
            }
            mark = ok ? "ok" : "WRONG";
        }
        char accepted[32] = "-";
        if (d.has_delay()) {
            std::snprintf(accepted, sizeof(accepted), "%d (%.2fms)", d.delay(), d.delay() * 1000.0 / kRate);
        }
        std::printf("  %6.2fs     %5d (%7.2fms)  %4.2fx  %5.1f  %-18s %s\n", start, s.last,
                    s.last * 1000.0 / kRate, s.peak_ratio, s.peak_snr, accepted, mark);
    }
    if (want >= 0) {
        std::printf("voting windows right %d, wrong %d; ", right, wrong);
    }
    std::printf("%ld windows, %ld without a peak, %ld skipped; accepted %d", d.stats().windows,
                d.stats().weak, d.stats().skipped, d.delay());
    if (want >= 0) {
        std::printf(", want %d", want);
    }
    std::printf("\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3) {
        double true_ms = -1.0;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--true-ms") == 0 && i + 1 < argc) {
                true_ms = std::atof(argv[++i]);
            } else {
                std::fprintf(stderr, "unknown option %s\n", argv[i]);
                return 2;
            }
        }
        return trace(argv[1], argv[2], true_ms);
    }
    if (argc != 1) {
        std::fprintf(stderr, "usage: %s [<ref.wav> <mic.wav> [--true-ms D]]\n", argv[0]);
        return 2;
    }
    test_delays();
    test_speech_like();
    test_double_talk();
    test_change();
    test_strong_reflection();
    test_no_echo();
    test_silence();
    test_prior();
    test_streaming();
    std::printf("\n%s\n", g_failures == 0 ? "all delay estimator checks passed"
                                          : "SOME DELAY ESTIMATOR CHECKS FAILED");
    return g_failures == 0 ? 0 : 1;
}
