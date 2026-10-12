// Passages read at several talker temperatures and seeds, for listening.
//
//     build/yoai-tts-sweep [--passages <file>] [--seeds <a,b,...>] [--whole] [--streaming-text]
//                          <talker.gguf> <tokenizer.gguf> <reference.spk> <out_dir>
//                          [temperature ...]
//
//   --passages  one passage per line, UTF-8, instead of the three below; blank
//               lines and lines starting with # are skipped. For comparing
//               ways of writing the same sentence, one variant per line.
//   --seeds     instead of 23, 101, 202, 303.
//   --whole     each passage as ONE piece, uncut, rather than with the
//               desktop's cutting.
//   --streaming-text
//               the talker reads the text one token per frame
//               (Reader::set_streaming_text) instead of all of it up front.
//   --eos-hold <n>
//               keep generating n frames past the first stop
//               (Reader::set_eos_hold).
//   --whole-reply
//               each passage as one generation fed its text through the
//               reader's streaming path (Reader::set_whole_reply), not cut.
//   --speaker <name>
//               a named speaker, for a custom_voice checkpoint.
//   --instruct-file <file>
//               a style instruction, UTF-8, from a file: a Windows command
//               line would hand Chinese over in the ANSI code page.
//   reference.spk may be "-" for none, which is what custom_voice needs.
//
// The question it answers is whether a hotter talker -- the sampler for the
// first codebook, which carries what is said and its rhythm -- makes a reply
// sound less recited, and at what temperature it starts to break. The WAVs are
// the answer; the table is only a map of where to listen:
//
//   pieces    how the passage was cut, with the desktop's settings
//   F0 st     spread of the pieces' mean pitch, in semitones: register
//   c/s       spread of the pieces' speaking rate, in characters a second: how
//             much the pace moves between pieces, which is what an even,
//             recited reading lacks. "-" when the cut here does not match the
//             reader's and the pieces' text is not known
//   s/char    seconds of speech per character
//   !         s/char more than 15% off the median of the same passage and
//             temperature: a take that ran on past its text, or stopped short
//             of it, is the likeliest reason. Listen to those first.
//
// Missing and misread words are not caught by any of this; they are what the
// listening is for.
//
// The talker's temperature only; the acoustic stack stays at its default, which
// was swept already -- narrowing it did not hold the register (qwen3_tts.hpp).
// Each piece is its own generation, anchored to the reference, exactly as a
// reply is read on the desktop.

#include "voice/prosody/prosody.hpp"
#include "voice/text/sentence_stream.hpp"
#include "voice/text/speakable_stream.hpp"
#include "voice/tts/reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using yoai::voice::Reader;
using yoai::voice::ReaderResult;
using yoai::voice::SplitOptions;

namespace {

// Three kinds of reply. The first mixes statements with a question and an
// exclamation, whose delivery should differ; the second is a practical answer
// full of numbers, where a misreading is easiest to hear; the third is an
// explanation, the long even stretch where a recited pace shows most.
const char* kPassages[] = {
    "说起黄浦江，很多人第一个想到的是外滩。其实这条江的故事比外滩长得多。"
    "它从淀山湖一带流出来，一路向东北，穿过上海市区，最后在吴淞口汇入长江。"
    "你知道吗？江上的轮渡到今天还在开。"
    "傍晚坐一趟，看着两岸的灯一盏盏亮起来，真的很美！",

    "我查了一下，明天上海多云转小雨，最高气温二十六度，最低十九度。"
    "早上出门的话，记得带把伞。另外，晚高峰地铁二号线可能会比较挤，"
    "你要是不赶时间，可以晚半小时再走。",

    "这个问题其实不难。简单来说，植物白天靠叶子里的叶绿素吸收阳光，"
    "把二氧化碳和水变成糖，同时放出氧气。到了晚上没有光，这个过程就停下来了，"
    "植物反而会像我们一样呼吸，消耗一点氧气。所以卧室里放太多花草，并不是一个好主意。",
};

const long long kSeeds[] = {23, 101, 202, 303};

struct Take {
    std::vector<float>       pcm;
    std::vector<std::size_t> starts;   // where each piece begins, in samples
    int                      last = -1;
};

bool on_audio(const float* pcm, std::size_t n, int sentence, bool last, void* user) {
    Take* t = static_cast<Take*>(user);
    if (last) {
        return true;
    }
    if (sentence != t->last) {
        t->starts.push_back(t->pcm.size());
        t->last = sentence;
    }
    t->pcm.insert(t->pcm.end(), pcm, pcm + n);
    return true;
}

std::size_t chars_of(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
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

bool write_wav(const std::string& path, const std::vector<float>& pcm, int rate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const unsigned bytes = static_cast<unsigned>(pcm.size() * 2);
    std::fwrite("RIFF", 1, 4, f);
    put32(f, 36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 1);
    put32(f, static_cast<unsigned>(rate));
    put32(f, static_cast<unsigned>(rate) * 2);
    put16(f, 2);
    put16(f, 16);
    std::fwrite("data", 1, 4, f);
    put32(f, bytes);
    for (std::size_t k = 0; k < pcm.size(); ++k) {
        float s = std::max(-1.0f, std::min(1.0f, pcm[k]));
        put16(f, static_cast<unsigned short>(static_cast<short>(s * 32767.0f)));
    }
    std::fclose(f);
    return true;
}

// One take's numbers, printed once its whole group is in and the median known.
struct Row {
    std::string name;
    long long   seed;
    int         pieces;
    double      speech;
    double      f0_st;
    std::string cps;
    double      spc;
};

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> passages(kPassages, kPassages + sizeof(kPassages) / sizeof(kPassages[0]));
    std::vector<long long>   seeds(kSeeds, kSeeds + sizeof(kSeeds) / sizeof(kSeeds[0]));
    std::vector<char*>       args;
    bool                     whole          = false;
    bool                     streaming_text = false;
    int                      eos_hold       = 0;
    bool                     whole_reply    = false;
    std::string              speaker;
    std::string              instruct;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--passages" && i + 1 < argc) {
            std::ifstream in(argv[++i], std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "cannot read %s\n", argv[i]);
                return 2;
            }
            passages.clear();
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
                if (line.empty() || line[0] == '#') continue;
                passages.push_back(line);
            }
        } else if (a == "--whole") {
            whole = true;
        } else if (a == "--streaming-text") {
            streaming_text = true;
        } else if (a == "--whole-reply") {
            whole_reply = true;
        } else if (a == "--speaker" && i + 1 < argc) {
            speaker = argv[++i];
        } else if (a == "--instruct-file" && i + 1 < argc) {
            std::ifstream in(argv[++i], std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "cannot read %s\n", argv[i]);
                return 2;
            }
            std::stringstream all;
            all << in.rdbuf();
            instruct = all.str();
            if (instruct.size() >= 3 && instruct.compare(0, 3, "\xEF\xBB\xBF") == 0) instruct.erase(0, 3);
            while (!instruct.empty() && (instruct.back() == '\n' || instruct.back() == '\r')) instruct.pop_back();
        } else if (a == "--eos-hold" && i + 1 < argc) {
            eos_hold = std::atoi(argv[++i]);
        } else if (a == "--seeds" && i + 1 < argc) {
            seeds.clear();
            std::stringstream list(argv[++i]);
            std::string one;
            while (std::getline(list, one, ',')) {
                if (!one.empty()) seeds.push_back(std::atoll(one.c_str()));
            }
        } else {
            args.push_back(argv[i]);
        }
    }
    if (args.size() < 4 || passages.empty() || seeds.empty()) {
        std::fprintf(stderr,
                     "usage: yoai-tts-sweep [--passages <file>] [--seeds <a,b,...>] [--whole] [--streaming-text] [--eos-hold <n>] [--whole-reply] <talker.gguf> "
                     "<tokenizer.gguf> <reference.spk> <out_dir> [temperature ...]\n");
        return 2;
    }
    std::vector<float> temps;
    for (std::size_t i = 4; i < args.size(); ++i) {
        temps.push_back(static_cast<float>(std::atof(args[i])));
    }
    if (temps.empty()) {
        temps.push_back(0.7f);
        temps.push_back(0.9f);
        temps.push_back(1.1f);
        temps.push_back(1.3f);
    }
    const std::string out_dir = args[3];

    if (!Reader::available()) {
        std::fprintf(stderr, "this build has no speech engine\n");
        return 1;
    }
    Reader reader;
    if (!reader.load(args[0], args[1], 1)) {
        std::fprintf(stderr, "load: %s\n", reader.last_error().c_str());
        return 1;
    }
    reader.set_speaker(speaker);
    reader.set_voice(instruct);
    if (std::string(args[2]) != "-" && !reader.load_reference_embedding(args[2])) {
        std::fprintf(stderr, "reference: %s\n", reader.last_error().c_str());
        return 1;
    }

    // The desktop's cutting, so what is heard here is what a reply sounds like.
    SplitOptions split;
    split.min_chars       = 12;
    split.max_chars       = 40;
    split.clause_cuts     = true;
    split.later_min_chars = 30;
    split.later_max_chars = 80;
    split.stop_chars      = 6;
    if (whole) {
        // Nothing ends a piece before the passage does.
        split.min_chars       = 100000;
        split.max_chars       = 100000;
        split.tail_chars      = 0;
        split.clause_cuts     = false;
        split.later_min_chars = 0;
        split.later_max_chars = 0;
        split.stop_chars      = 0;
    }
    reader.set_split(split);
    reader.set_streaming_text(streaming_text);
    reader.set_eos_hold(eos_hold);
    reader.set_whole_reply(whole_reply);

    for (std::size_t p = 0; p < passages.size(); ++p) {
        const std::string& passage = passages[p];
        const double chars = static_cast<double>(chars_of(passage));

        // The pieces as the reader will cut them: the spoken text, fed whole.
        std::vector<std::string> pieces;
        {
            yoai::voice::SentenceStream stream(split);
            pieces = stream.feed(yoai::voice::speakable(passage));
            const std::vector<std::string> rest = stream.finish();
            pieces.insert(pieces.end(), rest.begin(), rest.end());
        }
        std::printf("\npassage %d, cut into %d piece(s):\n", static_cast<int>(p),
                    static_cast<int>(pieces.size()));
        for (std::size_t k = 0; k < pieces.size(); ++k) {
            std::printf("  [%d] %s\n", static_cast<int>(k), pieces[k].c_str());
        }
        std::printf("%-6s %-5s %-7s %-7s %-7s %-7s %s\n", "temp", "seed", "pieces", "speech",
                    "F0 st", "c/s", "s/char");

        for (std::size_t t = 0; t < temps.size(); ++t) {
            yoai::voice::QttsSampling sampling;
            sampling.temperature = temps[t];
            reader.set_sampling(sampling);

            std::vector<Row> rows;
            for (std::size_t s = 0; s < seeds.size(); ++s) {
                reader.set_seed(seeds[s]);
                Take take;
                ReaderResult r;
                if (!reader.read(passage, &on_audio, &take, &r) || take.pcm.empty()) {
                    std::printf("%-6.2f %-5lld failed: %s\n", temps[t], seeds[s],
                                reader.last_error().c_str());
                    continue;
                }

                // Per piece: mean pitch and speaking rate, at the reader's cuts.
                const double rate = Reader::sample_rate();
                std::vector<std::pair<float, float> > segments;
                for (std::size_t k = 0; k < take.starts.size(); ++k) {
                    const std::size_t end =
                        (k + 1 < take.starts.size()) ? take.starts[k + 1] : take.pcm.size();
                    segments.push_back(std::make_pair(static_cast<float>(take.starts[k] / rate),
                                                      static_cast<float>(end / rate)));
                }
                double f0_lo = 0, f0_hi = 0;
                bool have_f0 = false;
                yoai::voice::Recording rec;
                yoai::voice::ProsodyOptions popt;
                if (yoai::voice::analyze_segments(&take.pcm[0], take.pcm.size(),
                                                  Reader::sample_rate(), segments, popt, &rec)) {
                    for (std::size_t k = 0; k < rec.utterances.size(); ++k) {
                        const double f = rec.utterances[k].f0_mean;
                        if (f <= 0) {
                            continue;
                        }
                        if (!have_f0 || f < f0_lo) f0_lo = f;
                        if (!have_f0 || f > f0_hi) f0_hi = f;
                        have_f0 = true;
                    }
                }

                // Rate needs each piece's text, which the sink does not carry.
                // The same cut was made above from the same text and options;
                // when it agrees with the reader's count, the characters are
                // the pieces'.
                char cps[16] = "-";
                if (pieces.size() == take.starts.size()) {
                    double lo = 0, hi = 0;
                    for (std::size_t k = 0; k < pieces.size(); ++k) {
                        const double secs = segments[k].second - segments[k].first;
                        const double c = secs > 0 ? chars_of(pieces[k]) / secs : 0;
                        if (k == 0 || c < lo) lo = c;
                        if (k == 0 || c > hi) hi = c;
                    }
                    std::snprintf(cps, sizeof(cps), "%.2f", hi - lo);
                }

                char name[64];
                std::snprintf(name, sizeof(name), "p%d-t%.2f-s%lld.wav", static_cast<int>(p),
                              temps[t], seeds[s]);
                write_wav(out_dir + "/" + name, take.pcm, Reader::sample_rate());

                Row row;
                row.name   = name;
                row.seed   = seeds[s];
                row.pieces = r.sentences;
                row.speech = static_cast<double>(take.pcm.size()) / rate;
                row.f0_st  = have_f0 && f0_lo > 0 ? 12.0 * std::log(f0_hi / f0_lo) / std::log(2.0)
                                                  : 0.0;
                row.cps    = cps;
                row.spc    = row.speech / chars;
                rows.push_back(row);
            }

            // Flagged against the median of this passage at this temperature,
            // so a passage that is simply read slower does not flag itself.
            std::vector<double> spcs;
            for (std::size_t k = 0; k < rows.size(); ++k) {
                spcs.push_back(rows[k].spc);
            }
            std::sort(spcs.begin(), spcs.end());
            const double median = spcs.empty() ? 0 : spcs[spcs.size() / 2];
            for (std::size_t k = 0; k < rows.size(); ++k) {
                const Row& w = rows[k];
                const bool odd = median > 0 && std::fabs(w.spc - median) / median > 0.15;
                std::printf("%-6.2f %-5lld %-7d %-7.2f %-7.2f %-7s %.3f %s %s\n", temps[t], w.seed,
                            w.pieces, w.speech, w.f0_st, w.cps.c_str(), w.spc, odd ? "!" : " ",
                            w.name.c_str());
            }
            std::fflush(stdout);
        }
    }
    return 0;
}
