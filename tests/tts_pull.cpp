// A passage read while it is still being written, against the same passage
// given whole.
//
//     build/yoai-tts-pull <talker.gguf> <tokenizer.gguf> <reference.spk> <out_dir>
//                         [chars_per_second ...]
//
// The question is whether one generation can follow a writer -- the on-device
// LLM, streaming a reply -- instead of the reply being cut into pieces that are
// each generated from the start. Qtts::say_pulled feeds the talker the text as
// it arrives, one token per frame, and a frame whose token has not arrived
// waits for it. Two things are checked:
//
//   same      the pulled audio is the whole-text audio, sample for sample. The
//             talker is given the same tokens in the same order either way, so
//             on the CPU anything else is a bug in how the text was cut or
//             appended. On a GPU it is not expected: the text is projected a
//             chunk at a time instead of in one batch, and a matmul of another
//             shape rounds differently, so the draw parts early -- and chunks
//             arrive at slightly different moments from run to run, so two runs
//             of the same row need not agree either.
//   starved   seconds a player that started at the first audio would have
//             spent with nothing to play. The talker reads 12.5 tokens a
//             second until the text is exhausted, so a writer slower than that
//             holds generation back.
//
// The writer releases one character at a time at a fixed rate. Chinese runs at
// roughly a token per character here, so a rate in characters a second is close
// to the LLM's tokens a second. Seed 101, talker temperature 1.3, a 2-frame eos
// hold: the desktop's settings.

#include "voice/tts/qwen3_tts.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using yoai::voice::Qtts;
using yoai::voice::QttsResult;
using yoai::voice::QttsSampling;

namespace {

typedef std::chrono::steady_clock clk;

const char* kPassages[] = {
    "我查了一下，明天上海多云转小雨，最高气温二十六度，最低十九度。"
    "早上出门的话，记得带把伞。另外，晚高峰地铁二号线可能会比较挤，"
    "你要是不赶时间，可以晚半小时再走。",

    "这个问题其实不难。简单来说，植物白天靠叶子里的叶绿素吸收阳光，"
    "把二氧化碳和水变成糖，同时放出氧气。到了晚上没有光，这个过程就停下来了，"
    "植物反而会像我们一样呼吸，消耗一点氧气。所以卧室里放太多花草，并不是一个好主意。",
};

double since(clk::time_point t0) {
    return std::chrono::duration<double>(clk::now() - t0).count();
}

// Where each chunk of audio arrived, and the audio itself.
struct Take {
    clk::time_point      t0;
    std::vector<float>   pcm;
    std::vector<double>  at;    // arrival, seconds from t0
    std::vector<size_t>  len;   // samples
};

bool on_audio(const float* pcm, size_t n, bool last, void* user) {
    Take* t = static_cast<Take*>(user);
    if (last || n == 0) {
        return true;
    }
    t->at.push_back(since(t->t0));
    t->len.push_back(n);
    t->pcm.insert(t->pcm.end(), pcm, pcm + n);
    return true;
}

// A player that starts once `prebuffer` seconds of audio have arrived (or
// all of it, if less) and then plays at real time: the seconds it spends with
// nothing to play, and when it starts in *start.
double starved(const Take& t, double prebuffer, double* start) {
    const double rate = Qtts::sample_rate();
    double       have = 0.0, total = 0.0;
    for (size_t k = 0; k < t.len.size(); ++k) {
        total += t.len[k] / rate;
    }
    size_t k0 = 0;
    for (; k0 < t.at.size(); ++k0) {
        have += t.len[k0] / rate;
        if (have >= std::min(prebuffer, total)) {
            break;
        }
    }
    if (k0 >= t.at.size()) {
        *start = 0.0;
        return 0.0;
    }
    *start = t.at[k0];
    double end = t.at[k0] + have, stall = 0.0;
    for (size_t k = k0 + 1; k < t.at.size(); ++k) {
        if (t.at[k] > end) {
            stall += t.at[k] - end;
        }
        end = std::max(end, t.at[k]) + t.len[k] / rate;
    }
    return stall;
}

// One row of the table.
void report(int p, const char* writer, const Take& t, const char* same, const char* file) {
    double s0 = 0.0, s5 = 0.0;
    const double st0 = starved(t, 0.0, &s0);
    const double st5 = starved(t, 0.5, &s5);
    std::printf("%-8d %-12s %-8.2f %-8.2f %-9.2f %-8.2f %-9.2f %-5s %s\n", p, writer,
                static_cast<double>(t.pcm.size()) / Qtts::sample_rate(), s0, st0, s5, st5, same, file);
}

// The writer: one UTF-8 character at a time, `rate` a second, from t0.
struct Writer {
    std::string             text;
    double                  rate;
    size_t                  released = 0;   // bytes the source may hand out
    size_t                  taken    = 0;   // bytes it has
    bool                    done     = false;
    std::mutex              mu;
    std::condition_variable cv;

    void run(clk::time_point t0) {
        size_t pos = 0, n = 0;
        while (pos < text.size()) {
            size_t len = 1;
            const unsigned char c = static_cast<unsigned char>(text[pos]);
            if (c >= 0xF0) len = 4; else if (c >= 0xE0) len = 3; else if (c >= 0xC0) len = 2;
            pos += len;
            ++n;
            std::this_thread::sleep_until(t0 + std::chrono::duration_cast<clk::duration>(
                                                   std::chrono::duration<double>(n / rate)));
            std::lock_guard<std::mutex> lock(mu);
            released = pos;
            cv.notify_all();
        }
        std::lock_guard<std::mutex> lock(mu);
        done = true;
        cv.notify_all();
    }
};

int pull(char* buf, int cap, void* user) {
    Writer* w = static_cast<Writer*>(user);
    std::unique_lock<std::mutex> lock(w->mu);
    w->cv.wait(lock, [w] { return w->taken < w->released || w->done; });
    if (w->taken >= w->released) {
        return 0;   // done, and everything handed out
    }
    const size_t n = std::min(static_cast<size_t>(cap), w->released - w->taken);
    std::memcpy(buf, w->text.data() + w->taken, n);
    w->taken += n;
    return static_cast<int>(n);
}

void put16(std::FILE* f, unsigned v) {
    const unsigned char b[2] = {static_cast<unsigned char>(v & 0xFF),
                                static_cast<unsigned char>((v >> 8) & 0xFF)};
    std::fwrite(b, 1, 2, f);
}

void put32(std::FILE* f, unsigned v) {
    put16(f, v & 0xFFFF);
    put16(f, v >> 16);
}

void write_wav(const std::string& path, const std::vector<float>& pcm) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return;
    }
    const unsigned rate  = static_cast<unsigned>(Qtts::sample_rate());
    const unsigned bytes = static_cast<unsigned>(pcm.size() * 2);
    std::fwrite("RIFF", 1, 4, f);
    put32(f, 36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 1);
    put32(f, rate);
    put32(f, rate * 2);
    put16(f, 2);
    put16(f, 16);
    std::fwrite("data", 1, 4, f);
    put32(f, bytes);
    for (size_t k = 0; k < pcm.size(); ++k) {
        const float s = std::max(-1.0f, std::min(1.0f, pcm[k]));
        put16(f, static_cast<unsigned short>(static_cast<short>(s * 32767.0f)));
    }
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: yoai-tts-pull <talker.gguf> <tokenizer.gguf> <reference.spk> "
                             "<out_dir> [chars_per_second ...]\n");
        return 2;
    }
    std::vector<double> rates;
    for (int i = 5; i < argc; ++i) {
        rates.push_back(std::atof(argv[i]));
    }
    if (rates.empty()) {
        rates.push_back(18.0);
        rates.push_back(9.0);
    }
    const std::string out_dir = argv[4];

    Qtts tts;
    if (!tts.load(argv[1], argv[2])) {
        std::fprintf(stderr, "load: %s\n", tts.last_error().c_str());
        return 1;
    }
    if (!tts.load_reference_embedding(argv[3])) {
        std::fprintf(stderr, "reference: %s\n", tts.last_error().c_str());
        return 1;
    }
    QttsSampling sampling;
    sampling.temperature = 1.3f;
    tts.set_sampling(sampling);
    tts.set_seed(101);
    tts.set_streaming(true);
    tts.set_streaming_text(true);
    tts.set_eos_hold(2);

    // The first synthesis on a GPU compiles its shaders, seconds of it, and
    // would be billed to whichever row came first.
    {
        Take warm;
        warm.t0 = clk::now();
        QttsResult rw;
        tts.say("\xE4\xBD\xA0\xE5\xA5\xBD\xE3\x80\x82", &on_audio, &warm, &rw);   // 你好。
    }

    // first / starved: a player that starts with the first chunk. +0.5: one
    // that waits for half a second of audio first, as the desktop's does.
    std::printf("%-8s %-12s %-8s %-8s %-9s %-8s %-9s %-5s %s\n", "passage", "writer", "speech", "first",
                "starved", "first+.5", "starved", "same", "file");
    for (size_t p = 0; p < sizeof(kPassages) / sizeof(kPassages[0]); ++p) {
        const std::string passage = kPassages[p];

        Take whole;
        whole.t0 = clk::now();
        QttsResult r;
        if (!tts.say(passage, &on_audio, &whole, &r)) {
            std::fprintf(stderr, "say: %s\n", tts.last_error().c_str());
            return 1;
        }
        char name[64];
        std::snprintf(name, sizeof(name), "p%d-whole.wav", static_cast<int>(p));
        write_wav(out_dir + "/" + name, whole.pcm);
        report(static_cast<int>(p), "all at once", whole, "-", name);

        for (size_t k = 0; k < rates.size(); ++k) {
            Writer w;
            w.text = passage;
            w.rate = rates[k];
            Take take;
            take.t0 = clk::now();
            std::thread writer(&Writer::run, &w, take.t0);
            QttsResult rp;
            const bool ok = tts.say_pulled("", &pull, &w, &on_audio, &take, &rp);
            writer.join();
            if (!ok) {
                std::fprintf(stderr, "say_pulled: %s\n", tts.last_error().c_str());
                return 1;
            }
            std::snprintf(name, sizeof(name), "p%d-pull-%.0f.wav", static_cast<int>(p), rates[k]);
            write_wav(out_dir + "/" + name, take.pcm);
            const bool same = take.pcm == whole.pcm;
            char label[32];
            std::snprintf(label, sizeof(label), "%.0f char/s", rates[k]);
            report(static_cast<int>(p), label, take, same ? "yes" : "no", name);
        }
        std::fflush(stdout);
    }
    return 0;
}
