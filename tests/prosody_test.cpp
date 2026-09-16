// The within-recording arousal profile, exercised on a workstation.
//
// No model and no GGUF: this half of the voice work is signal processing, which
// is the whole point of it -- see design/voice.md. So this runs anywhere the
// core builds.
//
//     build/bodyclock-prosody-test <audio.wav> [more.wav ...]
//
// The table it prints is meant to be compared, column by column, against the
// scratchpad Python that produced the result design/voice.md records. That
// comparison is the only check available here: what a recording means is not a
// fact this file knows, but "does the port agree with the thing that was
// validated" is, and the F0 and z columns are scale-invariant so the two agree
// exactly or they do not.
//
// The reference, on a 37.4 s monologue read at 16 kHz:
//
//     baseline F0 123.5 Hz (sd 11.1), 11 utterances
//     #0  at  0.1s  F0 123.3  z(F0) -0.02  z(rms)  2.62  z(spch)  2.53  arousal  1.40
//     #5  at 15.9s  F0 109.7  z(F0) -1.25  z(rms) -0.25  z(spch) -0.68  arousal -0.85
//
// #5 is the sentence where the speaker turns to his mother. Nothing here reads a
// word of the content.

#include "voice/prosody.hpp"

#include "wav.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

using bodyclock::voice::ArousalWeights;
using bodyclock::voice::Deviation;
using bodyclock::voice::ProsodyOptions;
using bodyclock::voice::Recording;
using bodyclock::voice::Utterance;

namespace {

// Five is the floor, and it is a real one: a z-score over three units is noise.
// Below it the honest output is nothing rather than a weak number.
const size_t kMinUtterances = 5;

void print_recording(const Recording& r, double elapsed) {
    std::printf("      baseline F0 %.1f Hz (sd %.1f), %zu utterances over %.1f s, "
                "analysed in %.2f s\n\n",
                r.baseline.f0_mean, r.baseline.f0_sd, r.utterances.size(),
                r.duration, elapsed);

    if (r.utterances.size() < kMinUtterances) {
        std::printf("      too few utterances to normalise against -- the right\n"
                    "      output here is nothing, not a weak number\n");
        return;
    }

    std::printf("        # %8s %7s %8s %8s %9s %8s %8s %9s\n",
                "start", "dur", "F0 Hz", "range st", "z(F0)", "z(rms)", "z(spch)", "arousal");
    std::printf("      %s\n", "------------------------------------------------------------------------------");
    for (size_t i = 0; i < r.utterances.size(); ++i) {
        const Utterance& u = r.utterances[i];
        const Deviation  d = bodyclock::voice::deviation(r, i);
        const float      a = bodyclock::voice::arousal(r, i);

        // Eight steps of block, the same shape the scratchpad tool drew, so the
        // two outputs can be read side by side.
        const char* blocks[8] = {"\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
                                 "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"};
        int step = static_cast<int>((a + 2.0f) / 4.0f * 8.0f);
        if (step < 0) step = 0;
        if (step > 7) step = 7;

        std::printf("      %3zu %7.1fs %6.1fs %8.1f %8.1f %9.2f %8.2f %8.2f %9.2f %s\n",
                    i, u.start, u.duration, u.f0_mean, u.f0_range_st,
                    d.f0, d.loudness, d.continuity, a, blocks[step]);
    }

    // The two the design rests on, named rather than left to be spotted in the
    // table: the claim is about where the extremes LAND, not about the spread.
    size_t low = 0, high = 0;
    for (size_t i = 1; i < r.utterances.size(); ++i) {
        if (bodyclock::voice::arousal(r, i) < bodyclock::voice::arousal(r, low))  low = i;
        if (bodyclock::voice::arousal(r, i) > bodyclock::voice::arousal(r, high)) high = i;
    }
    std::printf("\n      lowest  #%zu at %.1fs (%.2f)\n", low, r.utterances[low].start,
                bodyclock::voice::arousal(r, low));
    std::printf("      highest #%zu at %.1fs (%.2f)\n", high, r.utterances[high].start,
                bodyclock::voice::arousal(r, high));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <audio.wav> [more.wav ...]\n", argv[0]);
        return 2;
    }

    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        std::printf("%s\n", argv[i]);

        std::vector<float> pcm;
        int rate = 0;
        if (!bodyclock::testing::read_wav_16k_mono(argv[i], &pcm, &rate)) {
            ++failures;
            continue;
        }
        if (rate != 16000) {
            std::printf("      (resampled from %d Hz by the harness -- the numbers below\n"
                        "       will not match a tool that resampled differently)\n", rate);
        }

        Recording r;
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        const bool ok = bodyclock::voice::analyze(&pcm[0], pcm.size(), 16000,
                                                  ProsodyOptions(), &r);
        const double elapsed =
            std::chrono::duration_cast<std::chrono::duration<double> >(
                std::chrono::steady_clock::now() - t0).count();
        if (!ok) {
            std::printf("FAIL  analyze returned false\n");
            ++failures;
            continue;
        }
        print_recording(r, elapsed);
        std::printf("\n");
    }
    return failures ? 1 : 0;
}
