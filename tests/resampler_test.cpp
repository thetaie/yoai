// The resampler in voice/algorithms, on its own.
//
// With no arguments it checks what can be checked exactly or against a
// closed-form answer: tones in the passband come out as the same tone, tones
// above the output's Nyquist do not fold back, a constant stays constant, and
// the output does not depend on how the input was split.
//
//     build/yoai-resampler-test
//
// Given two paths it converts a WAV to 16 kHz, or to the rate given third, lined
// up with the input -- the filter's delay trimmed from the front, its tail
// flushed out the back -- so the two files can be listened to side by side.
// Converting the result back to the original rate leaves the original with only
// its top band missing, which separates what the resampler does from what the
// lower rate cannot hold.
//
//     build/yoai-resampler-test <in.wav> <out.wav> [out_rate]

#include "voice/algorithms/resampler.hpp"

#include "wav.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using yoai::voice::Resampler;

namespace {

const double kPi = 3.14159265358979323846;

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

std::vector<float> tone(double hz, double seconds, int rate, double amplitude) {
    const size_t n = static_cast<size_t>(seconds * rate);
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) {
        x[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * i / rate));
    }
    return x;
}

std::vector<float> run(Resampler& r, const std::vector<float>& x) {
    std::vector<float> y;
    r.process(x.empty() ? nullptr : &x[0], x.size(), &y);
    return y;
}

double db(double ratio) {
    return 20.0 * std::log10(ratio > 1e-30 ? ratio : 1e-30);
}

// Level of `y` against a full-scale tone of amplitude `a`, and, when `hz` is in
// the passband, how far `y` is from that tone delayed by the filter. Only the
// middle is measured: the first and last filter lengths are the filter filling
// and emptying, not the steady state.
struct ToneResult {
    double gain_db;
    double error_db;
};

ToneResult measure(const std::vector<float>& y, double hz, double a, int rate, int delay,
                   int margin) {
    // Against the ideal over the same stretch, not against a / sqrt(2): the
    // stretch is not a whole number of periods, and at 50 Hz the difference
    // reads as a gain error of several thousandths of a dB that is not there.
    double sig = 0.0, ref = 0.0, err = 0.0;
    for (size_t m = static_cast<size_t>(margin); m + margin < y.size(); ++m) {
        const double ideal = a * std::sin(2.0 * kPi * hz * (static_cast<double>(m) - delay) / rate);
        sig += static_cast<double>(y[m]) * y[m];
        ref += ideal * ideal;
        err += (y[m] - ideal) * (y[m] - ideal);
    }
    ToneResult r;
    r.gain_db  = db(std::sqrt(sig / ref));
    r.error_db = db(std::sqrt(err / ref));
    return r;
}

void test_24k_to_16k() {
    std::printf("24 kHz to 16 kHz\n");
    Resampler r(24000, 16000);
    check(r.valid(), "24000 -> 16000 is a valid ratio");
    std::printf("      delay %d output samples (%.3f ms)\n", r.delay(), 1000.0 * r.delay() / 16000);

    const int margin = 4 * r.delay();
    char what[160];

    // The passband: 0 to 7.2 kHz. Each tone must come out at its own level and
    // shape, delayed by exactly delay() samples.
    const double pass[] = {50, 440, 1000, 3000, 5000, 6500, 7000};
    for (size_t i = 0; i < sizeof(pass) / sizeof(pass[0]); ++i) {
        r.reset();
        const std::vector<float> y = run(r, tone(pass[i], 1.0, 24000, 0.5));
        const ToneResult t = measure(y, pass[i], 0.5, 16000, r.delay(), margin);
        std::snprintf(what, sizeof(what),
                      "%5.0f Hz passes: gain %+.4f dB, error %.1f dB", pass[i], t.gain_db,
                      t.error_db);
        check(std::fabs(t.gain_db) < 0.01 && t.error_db < -70.0, what);
    }

    // Above 8 kHz, which would fold back to 16 kHz minus itself. Nothing may.
    const double stop[] = {8200, 9000, 10000, 11500};
    for (size_t i = 0; i < sizeof(stop) / sizeof(stop[0]); ++i) {
        r.reset();
        const std::vector<float> y = run(r, tone(stop[i], 1.0, 24000, 0.5));
        const ToneResult t = measure(y, stop[i], 0.5, 16000, r.delay(), margin);
        std::snprintf(what, sizeof(what), "%5.0f Hz, which would alias to %4.0f Hz, is %.1f dB down",
                      stop[i], 16000 - stop[i], t.gain_db);
        check(t.gain_db < -75.0, what);
    }

    // A constant stays the same constant. Each phase is normalized to sum to
    // one, so this holds for every output sample, not just on average.
    {
        r.reset();
        const std::vector<float> y = run(r, std::vector<float>(24000, 0.25f));
        double worst = 0.0;
        for (size_t m = static_cast<size_t>(margin); m + margin < y.size(); ++m) {
            worst = std::max(worst, std::fabs(y[m] - 0.25));
        }
        std::snprintf(what, sizeof(what), "a constant 0.25 comes out as 0.25 (worst %.2e)", worst);
        check(worst < 1e-6, what);
    }

    // The count: output m exists once the input it reaches, floor(3m / 2), has
    // arrived.
    {
        r.reset();
        const size_t n = 24001;
        const std::vector<float> y = run(r, std::vector<float>(n, 0.0f));
        const size_t expect = (n * 2 - 1) / 3 + 1;
        std::snprintf(what, sizeof(what), "%zu samples in give %zu out (expected %zu)", n, y.size(),
                      expect);
        check(y.size() == expect, what);
    }

    // Split any way, the same bits. Chunk sizes from a fixed LCG, zero included.
    {
        std::vector<float> x(48000);
        uint32_t s = 12345;
        for (size_t i = 0; i < x.size(); ++i) {
            s = s * 1664525u + 1013904223u;
            x[i] = static_cast<float>((s >> 8) / 16777216.0 - 0.5) +
                   0.3f * static_cast<float>(std::sin(2.0 * kPi * 700.0 * i / 24000));
        }
        r.reset();
        const std::vector<float> whole = run(r, x);

        r.reset();
        std::vector<float> pieces;
        size_t at = 0;
        while (at < x.size()) {
            s = s * 1664525u + 1013904223u;
            size_t len = (s >> 16) % 700;
            if (len > x.size() - at) {
                len = x.size() - at;
            }
            r.process(len ? &x[at] : nullptr, len, &pieces);
            at += len;
        }
        const bool same = whole.size() == pieces.size() &&
                          std::memcmp(&whole[0], &pieces[0], whole.size() * sizeof(float)) == 0;
        check(same, "the output is the same bits however the input is split");
    }
}

void test_ratios() {
    std::printf("ratios\n");
    check(!Resampler(0, 16000).valid(), "a zero rate is refused");
    check(!Resampler(16001, 16000).valid(), "a ratio that does not reduce (16000/16001) is refused");
    check(Resampler(48000, 16000).valid(), "48000 -> 16000 is valid");
    check(Resampler(44100, 16000).valid(), "44100 -> 16000 is valid");

    Resampler bad(16001, 16000);
    std::vector<float> y;
    const std::vector<float> x(1000, 0.5f);
    bad.process(&x[0], x.size(), &y);
    check(y.empty(), "an invalid resampler emits nothing");

    // Downsampling by three: the case where one output can reach past what has
    // arrived, which the buffer trimming has to survive.
    Resampler r(48000, 16000);
    const std::vector<float> in = tone(1000, 1.0, 48000, 0.5);
    std::vector<float> out;
    for (size_t at = 0; at < in.size(); at += 1) {
        r.process(&in[at], 1, &out);
    }
    const ToneResult t = measure(out, 1000, 0.5, 16000, r.delay(), 4 * r.delay());
    char what[160];
    std::snprintf(what, sizeof(what),
                  "48 kHz to 16 kHz, one sample at a time: 1 kHz at %+.4f dB, error %.1f dB",
                  t.gain_db, t.error_db);
    check(std::fabs(t.gain_db) < 0.01 && t.error_db < -70.0, what);
}

int convert(const char* in_path, const char* out_path, int out_rate) {
    std::vector<float> x;
    int rate = 0;
    if (!yoai::testing::read_wav_mono(in_path, &x, &rate)) {
        return 1;
    }
    Resampler r(rate, out_rate);
    if (!r.valid()) {
        std::printf("cannot resample %d Hz to %d Hz\n", rate, out_rate);
        return 1;
    }

    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::vector<float> y;
    r.process(x.empty() ? nullptr : &x[0], x.size(), &y);
    // Enough silence to push the last real sample through the filter's delay.
    const std::vector<float> tail(static_cast<size_t>(
        (static_cast<int64_t>(r.delay()) + 2) * rate / out_rate + 1), 0.0f);
    r.process(&tail[0], tail.size(), &y);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // Lined up with the input: output delay() is where input 0 lands.
    size_t want = static_cast<size_t>(static_cast<int64_t>(x.size()) * out_rate / rate);
    const size_t lead = static_cast<size_t>(r.delay());
    if (y.size() < lead + want) {
        want = y.size() - lead;
    }
    std::vector<float> aligned(y.begin() + lead, y.begin() + lead + want);

    float peak = 0.0f;
    for (size_t i = 0; i < aligned.size(); ++i) {
        peak = std::max(peak, std::fabs(aligned[i]));
    }
    if (!yoai::testing::write_wav_mono16(out_path, aligned, out_rate)) {
        return 1;
    }
    const double seconds = static_cast<double>(x.size()) / rate;
    std::printf("%s: %d Hz, %.2f s\n", in_path, rate, seconds);
    std::printf("%s: %d Hz, %zu samples, peak %.3f%s\n", out_path, out_rate, aligned.size(), peak,
                peak >= 1.0f ? " -- clipped" : "");
    std::printf("took %.1f ms, RTF %.5f\n", took * 1000.0, took / seconds);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 || argc == 4) {
        const int out_rate = argc == 4 ? std::atoi(argv[3]) : 16000;
        return convert(argv[1], argv[2], out_rate);
    }
    if (argc != 1) {
        std::fprintf(stderr, "usage: %s [<in.wav> <out.wav> [out_rate, default 16000]]\n", argv[0]);
        return 2;
    }
    test_24k_to_16k();
    test_ratios();
    std::printf("\n%s\n", g_failures == 0 ? "all resampler checks passed"
                                          : "SOME RESAMPLER CHECKS FAILED");
    return g_failures == 0 ? 0 : 1;
}
