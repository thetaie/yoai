// yoai-asr -- speech recognition from the command line.
//
// Not a product: it exists so the ASR lane can be tried on a workstation
// against any WAV, without a phone or a test harness in the way. It drives
// voice/asr/sensevoice.hpp directly rather than the C ABI, because the class
// carries what a person checking a transcript wants to see -- token timestamps,
// the tags, and what the stray-capital filter took out -- and the ABI flattens
// most of that away.
//
//   yoai-asr <model.gguf> [options] <audio.wav> [more.wav ...]
//
// The model is loaded once and every file after it goes through the same
// instance, so a batch pays for the load a single time.
//
// A WAV that is not 16 kHz is resampled by tests/wav.hpp, linearly and with no
// anti-aliasing filter, and the tool says so: a transcript from a resampled
// file is a transcript of slightly different audio than a device would hand
// over.

#include "platform/log.hpp"
#include "voice/asr/sensevoice.hpp"
#include "wav.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

typedef std::chrono::steady_clock clk;

double seconds_since(clk::time_point start) {
    return std::chrono::duration<double>(clk::now() - start).count();
}

struct Options {
    std::string              model;
    std::vector<std::string> audio;
    int  threads = 0;       // 0 keeps SenseVoice's own default
    bool itn     = true;
    bool tokens  = false;
    bool quiet   = false;
    bool debug   = false;
};

void usage() {
    std::fprintf(stderr,
        "usage: yoai-asr <model.gguf> [options] <audio.wav> [more.wav ...]\n"
        "\n"
        "  -t N        encoder threads (default 4)\n"
        "  --no-itn    no inverse text normalization: numbers stay spoken words,\n"
        "              which is what a syllable count wants\n"
        "  --tokens    list every decoded token with its timestamp\n"
        "  -q          print only the text, one line per file\n"
        "  --debug     verbose core logging\n");
}

bool parse(int argc, char** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "-t") == 0 && i + 1 < argc) {
            o->threads = std::atoi(argv[++i]);
            if (o->threads <= 0) {
                std::fprintf(stderr, "-t wants a positive number\n");
                return false;
            }
        } else if (std::strcmp(a, "--no-itn") == 0) {
            o->itn = false;
        } else if (std::strcmp(a, "--tokens") == 0) {
            o->tokens = true;
        } else if (std::strcmp(a, "-q") == 0) {
            o->quiet = true;
        } else if (std::strcmp(a, "--debug") == 0) {
            o->debug = true;
        } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            return false;
        } else if (a[0] == '-' && a[1] != '\0') {
            std::fprintf(stderr, "unknown option %s\n", a);
            return false;
        } else if (o->model.empty()) {
            o->model = a;
        } else {
            o->audio.push_back(a);
        }
    }
    if (o->model.empty() || o->audio.empty()) {
        return false;
    }
    return true;
}

const char* or_dash(const std::string& s) {
    return s.empty() ? "-" : s.c_str();
}

void print_tokens(const yoai::voice::Transcript& r) {
    std::printf("  tokens\n");
    for (size_t i = 0; i < r.tokens.size(); ++i) {
        const yoai::voice::Token& t = r.tokens[i];
        // Negative frames are the query rows the tags come back on, which have
        // no position in the audio.
        char when[16];
        if (t.frame < 0) {
            std::snprintf(when, sizeof(when), "%7s", "-");
        } else {
            std::snprintf(when, sizeof(when), "%6.2fs", t.start);
        }
        std::printf("    %s  %6d  %s%s\n", when, t.id, t.piece.c_str(),
                    t.dropped ? "  (dropped: stray capital)" : "");
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, &o)) {
        usage();
        return 2;
    }
    yoai::platform::set_log_level(o.debug ? "debug" : "warn");

    if (!yoai::voice::SenseVoice::available()) {
        std::fprintf(stderr,
                     "this build has no recognition engine -- configure with -DYOAI_ASR=ON\n");
        return 1;
    }

    yoai::voice::SenseVoice sv;
    const clk::time_point load_started = clk::now();
    if (!sv.load(o.model)) {
        std::fprintf(stderr, "cannot load %s\n", o.model.c_str());
        return 1;
    }
    const double load_seconds = seconds_since(load_started);
    if (o.threads > 0) {
        sv.set_threads(o.threads);
    }
    if (!o.itn && !sv.set_inverse_text_normalization(false)) {
        std::fprintf(stderr, "--no-itn: this model has no text-normalization query slot\n");
        return 1;
    }
    if (!o.quiet) {
        std::fprintf(stderr, "loaded %s in %.2f s\n\n", o.model.c_str(), load_seconds);
    }

    int failures = 0;
    for (size_t a = 0; a < o.audio.size(); ++a) {
        const char* path = o.audio[a].c_str();
        std::vector<float> pcm;
        int rate = 0;
        if (!yoai::testing::read_wav_16k_mono(path, &pcm, &rate)) {
            ++failures;
            continue;
        }

        yoai::voice::Transcript r;
        const clk::time_point started = clk::now();
        const bool ok = sv.transcribe(pcm.empty() ? 0 : &pcm[0], pcm.size(), &r);
        const double elapsed = seconds_since(started);
        if (!ok) {
            std::fprintf(stderr, "%s: transcription failed (shorter than one analysis window?)\n",
                         path);
            ++failures;
            continue;
        }

        if (o.quiet) {
            std::printf("%s\n", r.text.c_str());
            continue;
        }

        std::printf("%s\n", path);
        if (rate != 16000) {
            std::printf("  note     resampled from %d Hz by the tool, linearly and unfiltered\n",
                        rate);
        }
        std::printf("  text     %s\n", r.text.empty() ? "(nothing)" : r.text.c_str());
        std::printf("  tags     language=%s emotion=%s event=%s itn=%s\n",
                    or_dash(r.language), or_dash(r.emotion), or_dash(r.event),
                    r.itn ? "yes" : "no");
        std::printf("  timing   %.2f s of audio in %.2f s (%.1fx real time)\n",
                    r.audio_seconds, elapsed,
                    elapsed > 0.0 ? r.audio_seconds / elapsed : 0.0);
        if (r.filtered_stray_capitals) {
            std::printf("  note     a run of isolated capital letters was filtered out\n");
        }
        if (o.tokens) {
            print_tokens(r);
        }
        std::printf("\n");
    }
    return failures == 0 ? 0 : 1;
}
