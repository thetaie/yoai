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

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

using bodyclock::voice::ArousalWeights;
using bodyclock::voice::Deviation;
using bodyclock::voice::ProsodyOptions;
using bodyclock::voice::Recording;
using bodyclock::voice::Utterance;

namespace {

// Three, lowered from five once the deviations were corrected to leave-one-out.
//
// Five was the right floor while an utterance sat inside its own baseline: at
// n=3 it contributed a third of the mean it was measured against, so a real turn
// came out a third too small. Excluding it restores the factor, which is what
// makes three usable rather than merely permitted.
//
// It is still a floor, and it is still enforced: two units have no distribution
// between them, and the honest output there is nothing rather than a weak number.
const size_t kMinUtterances = 3;

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

// Boundaries read from a file, one "start end" pair of seconds per line.
//
// The point of the file is that the boundaries come from somewhere this harness
// cannot produce: the transcript. SenseVoice's CTC gives every token the encoder
// frame that emitted it -- 60 ms -- so punctuation marks clause boundaries with
// timestamps, and clauses are finer than pauses. A ten-second note yields three
// or four pause-delimited units, below the floor where a deviation means
// anything; the same seconds cut at punctuation yield more.
std::vector<std::pair<float, float> > read_segments(const char* path) {
    std::vector<std::pair<float, float> > segs;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        float a = 0.0f, b = 0.0f;
        if (ls >> a >> b && b > a) {
            segs.push_back(std::make_pair(a, b));
        }
    }
    return segs;
}

// How many utterances survive at one setting, and how long the longest is.
//
// The second number is the one that matters. A cut that yields five units by
// splitting off four fragments and leaving one fifteen-second block has not
// segmented anything -- the block still averages a whole note into one "unit"
// whose pitch distribution spans the estimator's entire range.
void sweep(const std::vector<float>& pcm) {
    static const float pauses[]  = {0.40f, 0.30f, 0.25f, 0.20f, 0.15f, 0.12f, 0.10f};
    static const float silences[] = {0.04f, 0.08f, 0.12f, 0.16f};

    std::printf("      utterances (and longest, seconds) by min_pause x silence_fraction\n");
    std::printf("      %-8s", "pause\\sil");
    for (size_t s = 0; s < sizeof(silences) / sizeof(silences[0]); ++s) {
        std::printf("%14.2f", silences[s]);
    }
    std::printf("\n");

    for (size_t p = 0; p < sizeof(pauses) / sizeof(pauses[0]); ++p) {
        std::printf("      %-8.2f", pauses[p]);
        for (size_t s = 0; s < sizeof(silences) / sizeof(silences[0]); ++s) {
            ProsodyOptions o;
            o.min_pause       = pauses[p];
            o.silence_fraction = silences[s];
            Recording r;
            if (!bodyclock::voice::analyze(&pcm[0], pcm.size(), 16000, o, &r)) {
                std::printf("%14s", "-");
                continue;
            }
            float longest = 0.0f;
            for (size_t k = 0; k < r.utterances.size(); ++k) {
                if (r.utterances[k].duration > longest) {
                    longest = r.utterances[k].duration;
                }
            }
            char cell[32];
            std::snprintf(cell, sizeof(cell), "%zu (%.1f)", r.utterances.size(), longest);
            std::printf("%14s", cell);
        }
        std::printf("\n");
    }
    std::printf("\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s [--sweep] [--segments <file>] <audio.wav> [...]\n", argv[0]);
        return 2;
    }

    bool        do_sweep = false;
    const char* seg_file = 0;
    int         first    = 1;
    while (first < argc) {
        if (std::strcmp(argv[first], "--sweep") == 0) {
            do_sweep = true;
            ++first;
        } else if (std::strcmp(argv[first], "--segments") == 0 && first + 1 < argc) {
            seg_file = argv[first + 1];
            first += 2;
        } else {
            break;
        }
    }

    int failures = 0;
    for (int i = first; i < argc; ++i) {
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

        if (do_sweep) {
            sweep(pcm);
            continue;
        }

        Recording r;
        const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        bool ok = false;
        if (seg_file) {
            std::vector<std::pair<float, float> > segs = read_segments(seg_file);
            std::printf("      (%d boundaries supplied)\n", static_cast<int>(segs.size()));
            ok = bodyclock::voice::analyze_segments(&pcm[0], pcm.size(), 16000, segs,
                                                    ProsodyOptions(), &r);
        } else {
            ok = bodyclock::voice::analyze(&pcm[0], pcm.size(), 16000, ProsodyOptions(), &r);
        }
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
