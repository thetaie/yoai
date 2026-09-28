// The recognition lane, exercised on a workstation.
//
// Two modes, because the two halves fail differently.
//
// With no arguments it checks the FRONT END alone -- no model, no audio files,
// nothing to download. Those checks are exact: the filterbank and the low-frame-
// rate stacking are arithmetic with one right answer, and getting them subtly
// wrong is the failure that matters most here, because the encoder does not
// degrade gracefully on features that are merely close. It produces confident
// text that is wrong, which is indistinguishable from it working.
//
//     build/yoai-sensevoice-test
//
// Given a model and audio it transcribes, which needs both and cannot assert
// much: what a recording says is not a fact this file knows. It prints, and a
// person reads.
//
//     build/yoai-sensevoice-test <model.gguf> [-t threads] [-v] <audio.wav> [...]
//
// It prints the split between the front end and the encoder, which is what
// answers "is the filterbank worth hand-optimizing" -- and `-t` moves the only
// part of the pass that a change would actually be felt in.

#include "voice/asr/fbank/funasr.hpp"
#include "voice/asr/sensevoice.hpp"

#include "yoai.h"
#include "utils/log.hpp"

#include "wav.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using yoai::voice::FunAsrFbank;
using yoai::voice::FunAsrFbankOptions;
using yoai::voice::Transcript;
using yoai::voice::SenseVoice;
using yoai::voice::Token;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

double seconds_since(const std::chrono::steady_clock::time_point& t0) {
    using namespace std::chrono;
    return duration_cast<duration<double> >(steady_clock::now() - t0).count();
}

//------------------------------------------------------------------------------
// Front end
//------------------------------------------------------------------------------

// Kaldi's mel scale, repeated here on purpose: a test that imports the constant
// from the code it is testing cannot catch the constant being wrong.
double mel(double hz) {
    return 1127.0 * std::log(1.0 + hz / 700.0);
}

// `fade` ramps the amplitude from zero to full across the clip. A steady tone is
// the wrong signal for the stacking checks below: at 1 kHz into 16 kHz the period
// is exactly 16 samples and the hop is exactly 480, so frame 3 starts on the same
// phase as frame 0 and the two frames come out BITWISE identical. A check that
// two different frames differ then fails against correct code.
std::vector<float> tone(double hz, double seconds, int sample_rate, bool fade = false) {
    const size_t n = static_cast<size_t>(seconds * sample_rate);
    std::vector<float> pcm(n);
    for (size_t i = 0; i < n; ++i) {
        const double envelope = fade ? static_cast<double>(i) / n : 1.0;
        pcm[i] = static_cast<float>(
            0.5 * envelope * std::sin(2.0 * 3.14159265358979323846 * hz * i / sample_rate));
    }
    return pcm;
}

void test_front_end() {
    const FunAsrFbankOptions opt;
    FunAsrFbank fbank(opt);

    check(fbank.dim() == 560, "one feature row is 560 wide (7 stacked 80-bin frames)");

    // Nothing shorter than one analysis window produces a row. A caller with
    // 10 ms of audio should be told there is nothing there, not handed a frame.
    {
        std::vector<float> tiny = tone(1000.0, 0.01, opt.sample_rate);
        int frames = -1;
        std::vector<float> out = fbank.compute(&tiny[0], tiny.size(), &frames);
        check(frames == 0 && out.empty(), "audio shorter than one window yields no rows");
        check(fbank.frames_for(tiny.size()) == 0, "frames_for agrees that it yields none");
    }

    std::vector<float> pcm = tone(1000.0, 1.0, opt.sample_rate);
    int frames = 0;
    std::vector<float> feat = fbank.compute(&pcm[0], pcm.size(), &frames);

    check(frames > 0, "one second of audio produces rows");
    check(fbank.frames_for(pcm.size()) == frames,
          "frames_for predicts exactly what compute produces");
    check(feat.size() == static_cast<size_t>(frames) * fbank.dim(),
          "the buffer is frames x 560 with nothing left over");

    // 16000 samples, 25 ms window, 10 ms hop: (16000 - 400) / 160 + 1 = 98 mel
    // frames, stacked 7-wide every 6 -> ceil(98 / 6) = 17 rows. Spelled out
    // because an off-by-one here shifts every timestamp the transcript carries.
    check(frames == 17, "1.00 s of audio is 17 encoder frames");

    // A pure tone lands in the filter whose centre is nearest to it. Allow one
    // bin: the tone leaks into its neighbours and which side wins is a detail of
    // the window, but three bins away would mean the mel scale is wrong.
    {
        const double step = (mel(opt.high_freq) - mel(opt.low_freq)) / (opt.n_mel + 1);
        int expected = 0;
        double best = 1e18;
        for (int m = 0; m < opt.n_mel; ++m) {
            const double centre = mel(opt.low_freq) + (m + 1) * step;
            const double d = std::fabs(centre - mel(1000.0));
            if (d < best) {
                best = d;
                expected = m;
            }
        }
        int peak = 0;
        for (int m = 1; m < opt.n_mel; ++m) {
            if (feat[m] > feat[peak]) {
                peak = m;
            }
        }
        char what[96];
        std::snprintf(what, sizeof(what),
                      "a 1 kHz tone peaks at mel bin %d (expected %d)", peak, expected);
        check(std::abs(peak - expected) <= 1, what);
    }

    // The stacking, pinned exactly rather than approximately.
    //
    // Row i takes mel frames [i*6 .. i*6+6] out of a buffer with 3 copies of
    // frame 0 prepended. So row 0's first three blocks are all frame 0, and row
    // 0's block 6 and row 1's block 0 are the same frame. Both are byte
    // comparisons: if the pad count or the hop is wrong, they cannot pass.
    int ramp_frames = 0;
    std::vector<float> ramp_pcm = tone(1000.0, 1.0, opt.sample_rate, true);
    std::vector<float> ramp = fbank.compute(&ramp_pcm[0], ramp_pcm.size(), &ramp_frames);

    if (ramp_frames >= 2) {
        const int d = fbank.dim();
        const int n_mel = opt.n_mel;
        const float* row0 = &ramp[0];
        const float* row1 = &ramp[d];

        bool pad_ok = std::memcmp(row0, row0 + n_mel, n_mel * sizeof(float)) == 0 &&
                      std::memcmp(row0, row0 + 2 * n_mel, n_mel * sizeof(float)) == 0;
        check(pad_ok, "the first row repeats frame 0 three times (lfr_m/2 padding)");

        bool overlap_ok =
            std::memcmp(row0 + 6 * n_mel, row1, n_mel * sizeof(float)) == 0;
        check(overlap_ok, "consecutive rows overlap by one hop (row0 block 6 == row1 block 0)");

        // ... and that the two checks above are not passing because every frame
        // is identical, which would make them vacuous. The ramp guarantees the
        // frames differ; without this line a compute() that returned one frame
        // repeated would pass everything above.
        check(std::memcmp(row0, row0 + 6 * n_mel, n_mel * sizeof(float)) != 0,
              "distinct frames really are distinct (the checks above are not vacuous)");
    }
}

//------------------------------------------------------------------------------
// Transcription
//------------------------------------------------------------------------------

void print_result(const Transcript& r, double elapsed, double front_end) {
    std::printf("      text     %s\n", r.text.empty() ? "(nothing)" : r.text.c_str());
    std::printf("      tags     language=%s emotion=%s event=%s itn=%s\n",
                r.language.empty() ? "-" : r.language.c_str(),
                r.emotion.empty() ? "-" : r.emotion.c_str(),
                r.event.empty() ? "-" : r.event.c_str(),
                r.itn ? "yes" : "no");
    std::printf("      timing   %.2f s of audio in %.2f s -- %.0fx real time\n",
                r.audio_seconds, elapsed,
                elapsed > 0.0 ? r.audio_seconds / elapsed : 0.0);
    std::printf("      split    front end %.3f s (%.1f%%), encoder %.3f s\n",
                front_end, elapsed > 0.0 ? 100.0 * front_end / elapsed : 0.0,
                elapsed - front_end);
    if (r.filtered_stray_capitals) {
        std::printf("      NOTE     a run of isolated capital letters was filtered out\n");
    }

    // The first few timestamps, because they are the part that is new: greedy
    // CTC knows which encoder frame emitted each token, so a transcript carries
    // 60 ms alignment for free. Prosodic segmentation wants it.
    int shown = 0;
    std::printf("      first    ");
    for (size_t i = 0; i < r.tokens.size() && shown < 8; ++i) {
        if (r.tokens[i].frame < 0) {
            continue;   // a query token, not audio
        }
        std::printf("%.2fs:%s ", r.tokens[i].start, r.tokens[i].piece.c_str());
        ++shown;
    }
    std::printf("%s\n", shown ? "" : "(no audio tokens)");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("front end\n");
        test_front_end();
        std::printf("\n%s\n", g_failures ? "FAILED" : "all front-end checks passed");
        if (argc == 1) {
            std::printf(
                "\nto transcribe as well:\n"
                "  %s <model.gguf> [-t threads] <audio.wav> [more.wav ...]\n", argv[0]);
        }
        return g_failures ? 1 : 0;
    }

    std::printf("front end\n");
    test_front_end();
    std::printf("\n");

    if (!SenseVoice::available()) {
        std::printf("this build has no recognition engine -- configure with -DYOAI_ASR=ON\n");
        return g_failures ? 1 : 0;
    }

    // `-t N` sets the encoder's thread count. It is here because "should the
    // filterbank be hand-optimized" and "where does the time actually go" are
    // the same question, and this is the knob that moves the 98%.
    int  threads     = 4;
    bool no_itn      = false;
    bool through_abi = false;
    std::vector<const char*> audio;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            threads = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-v") == 0) {
            yoai::utils::set_log_level("debug");
        } else if (std::strcmp(argv[i], "--no-itn") == 0) {
            no_itn = true;
        } else if (std::strcmp(argv[i], "--abi") == 0) {
            through_abi = true;
        } else {
            audio.push_back(argv[i]);
        }
    }

    // --abi goes through the C entry points instead of the C++ classes: the same
    // path the HarmonyOS module takes, so the JSON the app will parse is checked
    // here rather than first seen on a device.
    if (through_abi) {
        if (yoai_voice_load(argv[1]) != 0) {
            std::printf("FAIL  yoai_voice_load(%s)\n", argv[1]);
            return 1;
        }
        if (no_itn && yoai_voice_set_itn(0) != 0) {
            std::printf("FAIL  yoai_voice_set_itn(0)\n");
            ++g_failures;
        }
        for (size_t a = 0; a < audio.size(); ++a) {
            std::printf("%s\n", audio[a]);
            std::vector<float> pcm;
            int rate = 0;
            if (!yoai::testing::read_wav_16k_mono(audio[a], &pcm, &rate)) {
                ++g_failures;
                continue;
            }
            // The ABI takes 16-bit samples, which is what a microphone hands
            // over; the harness reads floats, so it converts back here rather
            // than the core growing a second entry point for one test.
            std::vector<short> pcm16(pcm.size());
            for (size_t i = 0; i < pcm.size(); ++i) {
                float v = pcm[i] * 32768.0f;
                if (v > 32767.0f)  v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                pcm16[i] = static_cast<short>(v);
            }
            const char* json =
                yoai_voice_analyze(&pcm16[0], static_cast<int>(pcm16.size()), 16000);
            if (!json) {
                std::printf("FAIL  yoai_voice_analyze returned null\n");
                ++g_failures;
                continue;
            }
            std::printf("%s\n\n", json);
        }
        return g_failures ? 1 : 0;
    }

    SenseVoice sv;
    {
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        if (!sv.load(argv[1])) {
            std::printf("FAIL  could not load %s\n", argv[1]);
            return 1;
        }
        sv.set_threads(threads);
        if (no_itn && !sv.set_inverse_text_normalization(false)) {
            std::printf("FAIL  --no-itn: the model has no text-normalization query slot\n");
            ++g_failures;
        }
        std::printf("model %s loaded in %.2f s, %d threads, itn %s\n\n", argv[1],
                    seconds_since(t0), threads, no_itn ? "off" : "on");
    }

    for (size_t a = 0; a < audio.size(); ++a) {
        std::printf("%s\n", audio[a]);
        std::vector<float> pcm;
        int rate = 0;
        if (!yoai::testing::read_wav_16k_mono(audio[a], &pcm, &rate)) {
            ++g_failures;
            continue;
        }
        if (rate != 16000) {
            std::printf("      (resampled from %d Hz by the harness)\n", rate);
        }

        // The front end, timed on its own before the real call. transcribe()
        // does this work again internally; the point is the SPLIT -- how much of
        // a pass is the filterbank and how much is the encoder -- which is what
        // decides whether hand-optimizing the filterbank would buy anything.
        double front_end = 0.0;
        {
            FunAsrFbank probe;
            int probe_frames = 0;
            const std::chrono::steady_clock::time_point tf = std::chrono::steady_clock::now();
            probe.compute(&pcm[0], pcm.size(), &probe_frames);
            front_end = seconds_since(tf);
        }

        Transcript r;
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        const bool ok = sv.transcribe(&pcm[0], pcm.size(), &r);
        const double elapsed = seconds_since(t0);
        if (!ok) {
            std::printf("FAIL  transcribe returned false\n");
            ++g_failures;
            continue;
        }
        print_result(r, elapsed, front_end);
        std::printf("\n");
    }

    return g_failures ? 1 : 0;
}
