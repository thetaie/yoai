// The echo canceller in voice/aec, against echo whose every component is
// known.
//
// A synthetic microphone is the sum of three things this file made: the
// reference through an echo path, someone talking, and noise. Because each is
// known, the residual echo is known exactly -- the output minus the talker and
// the noise -- and the echo return loss enhancement (ERLE) can be measured
// while someone talks over it, which a real recording cannot give.
//
// With no arguments: white noise through a random path, then the same with a
// talker over it, a path that moves, and the output against its own streaming.
//
//     build/yoai-echo-canceller-test
//
// With two WAVs: the first is the reply being played, the second the person
// talking over it. Both are brought to 16 kHz by voice/algorithms/resampler,
// the reply is played into a synthetic room, and everything is written to
// <out_dir> to be listened to: mic.wav, out.wav, echo.wav, near.wav and
// residual.wav (what is left of the echo in out.wav).
//
//     build/yoai-echo-canceller-test <reply.wav> <talker.wav> <out_dir>
//         [--echo-db X]   echo against the talker, active level, dB (default 6)
//         [--at S]        when the talker starts, seconds (default 10)
//         [--delay-ms D]  output and input buffering ahead of the room (default 30)
//         [--distort]     a loudspeaker that saturates, which no linear filter models

#include "voice/aec/echo_canceller.hpp"
#include "voice/algorithms/resampler.hpp"

#include "wav.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using yoai::voice::EchoCanceller;
using yoai::voice::EchoCancellerOptions;
using yoai::voice::Resampler;

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

// Deterministic noise, so every run of this file sees the same signals.
struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    double uniform() {                       // [0, 1)
        s = s * 1664525u + 1013904223u;
        return (s >> 8) / 16777216.0;
    }
    double gauss() {
        const double u = std::max(uniform(), 1e-12);
        const double v = uniform();
        return std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * kPi * v);
    }
};

// The canceller removes DC from the microphone first: y[n] = x[n] - x[n-1] +
// 0.995 y[n-1]. The same filter is applied here to the talker and the noise so
// that out - talker - noise is the residual echo and nothing else. The pole is
// the canceller's; if it changes there it changes here.
std::vector<float> dc_notch(const std::vector<float>& x) {
    std::vector<float> y(x.size());
    float in = 0.0f, out = 0.0f;
    for (size_t i = 0; i < x.size(); ++i) {
        const float v = out * 0.995f + x[i] - in;
        in = x[i];
        out = v;
        y[i] = v;
    }
    return y;
}

// A room: `delay_ms` of nothing -- output and input buffering -- then a direct
// path and a reverberant tail decaying 60 dB over `rt60_ms`, cut at `len_ms`.
std::vector<float> room(double delay_ms, double rt60_ms, double len_ms, uint32_t seed) {
    Lcg rng(seed);
    const int delay = static_cast<int>(delay_ms * kRate / 1000);
    const int len   = static_cast<int>(len_ms * kRate / 1000);
    std::vector<float> h(static_cast<size_t>(delay + len), 0.0f);
    h[delay] = 1.0f;
    const double decay = 6.9078 / (rt60_ms * kRate / 1000);   // ln(1000) per rt60
    for (int i = 1; i < len; ++i) {
        h[delay + i] = static_cast<float>(0.3 * rng.gauss() * std::exp(-decay * i));
    }
    return h;
}

std::vector<float> convolve(const std::vector<float>& x, const std::vector<float>& h) {
    std::vector<float> y(x.size(), 0.0f);
    // Only the non-zero taps: a room's bulk delay is most of its length.
    std::vector<size_t> nz;
    for (size_t j = 0; j < h.size(); ++j) {
        if (h[j] != 0.0f) {
            nz.push_back(j);
        }
    }
    for (size_t i = 0; i < x.size(); ++i) {
        double acc = 0.0;
        for (size_t t = 0; t < nz.size(); ++t) {
            const size_t j = nz[t];
            if (j > i) {
                break;
            }
            acc += static_cast<double>(h[j]) * x[i - j];
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

double db(double p) {
    return 10.0 * std::log10(p > 1e-30 ? p : 1e-30);
}

std::vector<float> run(EchoCanceller& aec, const std::vector<float>& mic, const std::vector<float>& ref) {
    std::vector<float> out;
    aec.process(&mic[0], &ref[0], mic.size(), &out);
    return out;
}

// ERLE over [from, to): the echo at the microphone against what is left of it.
double erle(const std::vector<float>& echo, const std::vector<float>& residual, size_t from, size_t to) {
    return db(power(echo, from, to)) - db(power(residual, from, to));
}

//------------------------------------------------------------------------------
// Checks
//------------------------------------------------------------------------------

std::vector<float> white(size_t n, double amplitude, uint32_t seed) {
    Lcg rng(seed);
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) {
        x[i] = static_cast<float>(amplitude * (2.0 * rng.uniform() - 1.0));
    }
    return x;
}

void test_white_noise() {
    std::printf("white noise through a 145 ms path\n");
    const size_t n = 10 * kRate;
    const std::vector<float> ref   = white(n, 0.3, 1);
    const std::vector<float> path  = room(20, 150, 125, 2);
    const std::vector<float> raw   = convolve(ref, path);
    const std::vector<float> echo  = dc_notch(raw);
    const std::vector<float> noise = white(n, 1e-4, 3);
    std::vector<float> mic(n);
    for (size_t i = 0; i < n; ++i) {
        mic[i] = raw[i] + noise[i];
    }

    EchoCanceller aec;
    const std::vector<float> out = run(aec, mic, ref);
    const std::vector<float> nn = dc_notch(noise);
    std::vector<float> residual(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        residual[i] = out[i] - nn[i];
    }
    std::printf("      %d partitions of %d, %d ms\n", aec.partitions(), aec.options().block,
                aec.options().filter_ms);
    std::printf("      ERLE by second:");
    for (int s = 0; s < 10; ++s) {
        std::printf(" %.0f", erle(echo, residual, s * kRate, (s + 1) * kRate));
    }
    std::printf(" dB\n");
    char what[160];
    const double last = erle(echo, residual, 8 * kRate, 10 * kRate);
    std::snprintf(what, sizeof(what), "converged: ERLE %.1f dB over the last two seconds", last);
    check(last > 30.0, what);
    check(aec.stats().adapted, "past the bootstrap");
    check(aec.stats().resets == 0, "never diverged");
}

void test_double_talk() {
    std::printf("the same, with someone talking over it from 6 s to 8 s at the echo's level\n");
    const size_t n = 12 * kRate;
    const std::vector<float> ref  = white(n, 0.3, 1);
    const std::vector<float> path = room(20, 150, 125, 2);
    const std::vector<float> raw  = convolve(ref, path);
    const std::vector<float> echo = dc_notch(raw);
    const double echo_rms = std::sqrt(power(raw, 0, n));
    std::vector<float> near = white(n, echo_rms * std::sqrt(3.0), 4);
    for (size_t i = 0; i < n; ++i) {
        if (i < 6 * static_cast<size_t>(kRate) || i >= 8 * static_cast<size_t>(kRate)) {
            near[i] = 0.0f;
        }
    }
    std::vector<float> mic(n);
    for (size_t i = 0; i < n; ++i) {
        mic[i] = raw[i] + near[i];
    }

    EchoCanceller aec;
    const std::vector<float> out = run(aec, mic, ref);
    const std::vector<float> nn = dc_notch(near);
    std::vector<float> residual(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        residual[i] = out[i] - nn[i];
    }
    const double before = erle(echo, residual, 5 * kRate, 6 * kRate);
    const double during = erle(echo, residual, 6 * kRate, 8 * kRate);
    const double after  = erle(echo, residual, 10 * kRate, 12 * kRate);
    std::printf("      ERLE before %.1f, during %.1f, after %.1f dB\n", before, during, after);
    char what[160];
    std::snprintf(what, sizeof(what), "the talker does not undo the filter: %.1f dB while talking",
                  during);
    check(during > 20.0, what);
    check(after > 30.0, "and it is still converged afterwards");
}

void test_path_moves() {
    std::printf("the path moves at 8 s: 5 ms more delay, another room\n");
    const size_t n = 16 * kRate;
    const size_t cut = 8 * kRate;
    const std::vector<float> ref = white(n, 0.3, 1);
    const std::vector<float> a = convolve(ref, room(20, 150, 125, 2));
    const std::vector<float> b = convolve(ref, room(25, 150, 125, 5));
    std::vector<float> raw(n);
    for (size_t i = 0; i < n; ++i) {
        raw[i] = i < cut ? a[i] : b[i];
    }
    const std::vector<float> echo = dc_notch(raw);

    EchoCanceller aec;
    const std::vector<float> out = run(aec, raw, ref);
    std::printf("      ERLE by second:");
    for (int s = 0; s < 16; ++s) {
        std::printf(" %.0f", erle(echo, out, s * kRate, (s + 1) * kRate));
    }
    std::printf(" dB\n");
    const double back = erle(echo, out, 14 * kRate, 16 * kRate);
    char what[160];
    std::snprintf(what, sizeof(what), "re-converged within six seconds: %.1f dB", back);
    check(back > 25.0, what);
}

void test_streaming() {
    std::printf("streaming\n");
    const size_t n = 3 * kRate;
    const std::vector<float> ref = white(n, 0.3, 7);
    const std::vector<float> mic = convolve(ref, room(10, 100, 60, 8));

    EchoCanceller whole_aec;
    const std::vector<float> whole = run(whole_aec, mic, ref);

    EchoCanceller split_aec;
    std::vector<float> pieces;
    Lcg rng(9);
    size_t at = 0;
    while (at < n) {
        size_t len = static_cast<size_t>(rng.uniform() * 700);
        len = std::min(len, n - at);
        split_aec.process(len ? &mic[at] : nullptr, len ? &ref[at] : nullptr, len, &pieces);
        at += len;
    }
    check(whole.size() == n / 256 * 256, "output comes in whole blocks, one sample per input");
    const bool same = whole.size() == pieces.size() &&
                      std::memcmp(&whole[0], &pieces[0], whole.size() * sizeof(float)) == 0;
    check(same, "the output is the same bits however the input is split");
}

//------------------------------------------------------------------------------
// Two WAVs, for listening
//------------------------------------------------------------------------------

bool load_16k(const char* path, std::vector<float>* out) {
    std::vector<float> x;
    int rate = 0;
    if (!yoai::testing::read_wav_mono(path, &x, &rate)) {
        return false;
    }
    if (rate == kRate) {
        out->swap(x);
        return true;
    }
    Resampler r(rate, kRate);
    if (!r.valid()) {
        std::printf("cannot resample %s from %d Hz\n", path, rate);
        return false;
    }
    std::vector<float> y;
    r.process(&x[0], x.size(), &y);
    const std::vector<float> tail(static_cast<size_t>((r.delay() + 2) * rate / kRate + 1), 0.0f);
    r.process(&tail[0], tail.size(), &y);
    const size_t lead = static_cast<size_t>(r.delay());
    const size_t want = std::min(static_cast<size_t>(static_cast<int64_t>(x.size()) * kRate / rate),
                                 y.size() - lead);
    out->assign(y.begin() + lead, y.begin() + lead + want);
    return true;
}

// Mean square over the frames louder than 8% of the loudest 20 ms frame, so
// pauses do not dilute a level. The same rule the earlier mixing test used.
double active_power(const std::vector<float>& x) {
    const size_t f = kRate / 50;
    std::vector<double> frames;
    for (size_t i = 0; i + f <= x.size(); i += f) {
        frames.push_back(power(x, i, i + f));
    }
    double top = 0.0;
    for (size_t i = 0; i < frames.size(); ++i) {
        top = std::max(top, frames[i]);
    }
    double s = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i] > 0.0064 * top) {   // 8% in amplitude
            s += frames[i];
            ++n;
        }
    }
    return n ? s / n : 0.0;
}

int listen(const char* reply_path, const char* talker_path, const std::string& dir, double echo_db,
           double at_s, bool distort, double delay_ms) {
    std::vector<float> ref, talker;
    if (!load_16k(reply_path, &ref) || !load_16k(talker_path, &talker)) {
        return 1;
    }
    const size_t n = ref.size();

    // The loudspeaker, then the room: `delay_ms` of buffering (30 by default), a
    // 250 ms RT60 tail cut at 150 ms. 180 ms in all by default, inside the
    // filter's 256; past 106 ms of buffering the tail starts to fall outside.
    std::vector<float> speaker(ref);
    if (distort) {
        for (size_t i = 0; i < n; ++i) {
            speaker[i] = static_cast<float>(std::tanh(2.0 * speaker[i]) / 2.0);
        }
    }
    std::vector<float> echo = convolve(speaker, room(delay_ms, 250, 150, 11));

    // The talker, placed, and the echo scaled against the talker's level.
    std::vector<float> near(n, 0.0f);
    const size_t start = static_cast<size_t>(at_s * kRate);
    for (size_t i = 0; i < talker.size() && start + i < n; ++i) {
        near[start + i] = talker[i];
    }
    const double g = std::sqrt(active_power(talker) / active_power(echo) * std::pow(10.0, echo_db / 10.0));
    for (size_t i = 0; i < n; ++i) {
        echo[i] = static_cast<float>(echo[i] * g);
    }
    const std::vector<float> noise = white(n, 1e-3 * std::sqrt(3.0), 21);   // -60 dBFS
    std::vector<float> mic(n);
    for (size_t i = 0; i < n; ++i) {
        mic[i] = echo[i] + near[i] + noise[i];
    }

    EchoCanceller aec;
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    const std::vector<float> out = run(aec, mic, ref);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::vector<float> clean(n);
    for (size_t i = 0; i < n; ++i) {
        clean[i] = near[i] + noise[i];
    }
    const std::vector<float> clean_n = dc_notch(clean);
    const std::vector<float> echo_n  = dc_notch(echo);
    std::vector<float> residual(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        residual[i] = out[i] - clean_n[i];
    }

    std::printf("reply %s, %.1f s; talker %s, %.1f s from %.1f s\n", reply_path,
                static_cast<double>(n) / kRate, talker_path,
                static_cast<double>(talker.size()) / kRate, at_s);
    std::printf("echo %+.0f dB against the talker, %.0f ms of buffering%s; took %.2f s, RTF %.4f\n",
                echo_db, delay_ms, distort ? ", loudspeaker saturating" : "", took,
                took * kRate / n);
    std::printf("\n   s   echo dB  left dB   ERLE   talker\n");
    const size_t talker_end = start + talker.size();
    for (size_t s = 0; (s + 1) * kRate <= out.size(); ++s) {
        const size_t a = s * kRate, b = (s + 1) * kRate;
        const bool talking = a < talker_end && b > start;
        std::printf("  %2zu   %6.1f   %6.1f   %5.1f   %s\n", s, db(power(echo_n, a, b)),
                    db(power(residual, a, b)), erle(echo_n, residual, a, b),
                    talking ? "talking" : "");
    }
    std::printf("\nadapted %s, leak %.3f, resets %d\n", aec.stats().adapted ? "yes" : "no",
                aec.stats().leak, aec.stats().resets);

    const bool ok = yoai::testing::write_wav_mono16((dir + "/mic.wav").c_str(), mic, kRate) &&
                    yoai::testing::write_wav_mono16((dir + "/out.wav").c_str(), out, kRate) &&
                    yoai::testing::write_wav_mono16((dir + "/echo.wav").c_str(), echo, kRate) &&
                    yoai::testing::write_wav_mono16((dir + "/near.wav").c_str(), clean, kRate) &&
                    yoai::testing::write_wav_mono16((dir + "/residual.wav").c_str(), residual, kRate);
    if (ok) {
        std::printf("wrote mic, out, echo, near, residual .wav to %s\n", dir.c_str());
    }
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4) {
        double echo_db = 6.0, at_s = 10.0, delay_ms = 30.0;
        bool distort = false;
        for (int i = 4; i < argc; ++i) {
            if (std::strcmp(argv[i], "--echo-db") == 0 && i + 1 < argc) {
                echo_db = std::atof(argv[++i]);
            } else if (std::strcmp(argv[i], "--at") == 0 && i + 1 < argc) {
                at_s = std::atof(argv[++i]);
            } else if (std::strcmp(argv[i], "--delay-ms") == 0 && i + 1 < argc) {
                delay_ms = std::atof(argv[++i]);
            } else if (std::strcmp(argv[i], "--distort") == 0) {
                distort = true;
            } else {
                std::fprintf(stderr, "unknown option %s\n", argv[i]);
                return 2;
            }
        }
        return listen(argv[1], argv[2], argv[3], echo_db, at_s, distort, delay_ms);
    }
    if (argc != 1) {
        std::fprintf(stderr,
                     "usage: %s [<reply.wav> <talker.wav> <out_dir> [--echo-db X] [--at S] "
                     "[--delay-ms D] [--distort]]\n",
                     argv[0]);
        return 2;
    }
    test_white_noise();
    test_double_talk();
    test_path_moves();
    test_streaming();
    std::printf("\n%s\n", g_failures == 0 ? "all echo canceller checks passed"
                                          : "SOME ECHO CANCELLER CHECKS FAILED");
    return g_failures == 0 ? 0 : 1;
}
