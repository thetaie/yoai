// Whether a reading style survives the trip into the Base checkpoint.
//
//     build/yoai-tts-style [--speakers <a,b,...>] [--ref-seeds <a,b,...>] [--seeds <a,b,...>]
//                          [--styles <a,b,...>] [--own <voice.spk>] [--alphas <a,b,...>]
//                          [--no-ab] [--refs-only] [--directions <dir>]
//                          <customvoice.gguf> <base.gguf> <tokenizer.gguf> <out_dir>
//
//   --speakers    the CustomVoice speakers who read the references (default serena)
//   --ref-seeds   the seeds each of them reads with (default 101)
//   --seeds       the seeds Base reads the test passage with (default 23, 101, 202)
//   --styles      which of kStyles to use, by name; calm is always included,
//                 first, and `--styles calm` is calm alone
//   --own         a Base voice file, to try each style as a direction on it
//   --alphas      how far along each direction to go on --own (default 1)
//   --no-ab       skip modes A and B, for a run that is about --own
//   --refs-only   stop once the references are read and their voice files
//                 written: the raw material for directions made elsewhere
//   --directions  read direction-<style>.f32 from this directory instead of
//                 making references; needs --own, and runs only that part
//
// CustomVoice can be asked HOW to read -- 语气轻快，开心 -- but only as one of
// its built-in speakers. Base reads as anyone a recording gives it and takes no
// instruction at all. design/voice.md ("Emotion and pace have no vector") names
// two ways a style might still reach Base; this measures both, and a third that
// follows from the first:
//
//   references  CustomVoice reads one passage in each style, so within one
//               speaker and seed the style changes and the speaker does not.
//   mode A      each reference becomes a voice file, and Base reads a DIFFERENT
//               passage from it. If the vector carries the style, the reading
//               moves the way the reference did. The first speaker's first seed.
//   mode B      each reference and its transcript go in the prompt, and Base
//               continues from them. The same references as mode A.
//   own         with --own: own + alpha * direction, mode A. The style as a
//               direction, on a voice that never read in it.
//
// A style's direction is emb(style) - emb(calm), taken within one speaker and
// one seed and averaged over all of them: the average keeps what the styles
// share and drops what belongs to one speaker or one take. How much they share
// is printed as `agree`, the mean cosine between the single-pair differences
// that went into it. Each direction is written as direction-<style>.f32, raw f32
// like a .spk, for the talker width it was extracted with.
//
// The references are synthesized, not recorded: clean, labeled styles test the
// mechanism. A real speaker's takes are the step after, if anything carries.
//
// Per take:
//   c/s    characters a second, over the span from the first sound to the last
//   F0     mean pitch, in semitones above 100 Hz
//   range  pitch from its 10th to its 90th percentile, in semitones
//   level  mean RMS, in dB
//   pause  the share of that span that is silence
//
// A style is carried when a Base row moves away from its own calm row the way
// the reference moved from the reference calm, by more than the seeds move it.
// The last tables put those side by side. The WAVs are what to listen to.

#include "voice/prosody/prosody.hpp"
#include "voice/tts/qwen3_tts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using yoai::voice::Qtts;
using yoai::voice::QttsResult;
using yoai::voice::QttsSampling;

namespace {

// The first four are the desktop's (STYLES in examples/desktop/src/main/reader.ts);
// change them together. The last two are pace alone, which the design note
// expects a pooled speaker vector to lose and a reference in context to keep.
struct Style {
    const char* name;
    const char* instruct;
};

const Style kStyles[] = {
    {"calm", "语气平静。"},
    {"caring", "语气关切，温柔。"},
    {"cheerful", "语气轻快，开心。"},
    {"sorry", "语气抱歉，诚恳。"},
    {"fast", "语速很快。"},
    {"slow", "语速很慢。"},
};
const int kStyleCount = static_cast<int>(sizeof(kStyles) / sizeof(kStyles[0]));

// What every reference says: plain events, so the style is in the delivery
// and not in the words. About thirteen seconds read calmly.
const char* kReferenceText =
    "今天下午我去了一趟超市，买了些水果和面包。"
    "回来的路上下起了小雨，我就在路边的屋檐下等了一会儿，等雨小了才走回家。";

// What Base reads under each condition. Different from the reference, so that a
// copy of the reference's audio cannot pass for carrying its style.
const char* kTestText =
    "我查了一下，明天上海多云转小雨，最高气温二十六度。"
    "早上出门的话，记得带把伞。";

// The desktop's reading settings (examples/desktop/src/main/reader.ts and tts.ts).
void apply_reading(Qtts* tts) {
    QttsSampling sampling;
    sampling.temperature = 1.3f;
    tts->set_sampling(sampling);
    tts->set_streaming(false);
    tts->set_streaming_text(true);
    tts->set_eos_hold(2);
}

bool collect(const float* pcm, std::size_t n, bool last, void* user) {
    (void) last;
    std::vector<float>* out = static_cast<std::vector<float>*>(user);
    out->insert(out->end(), pcm, pcm + n);
    return true;
}

std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream list(s);
    std::string one;
    while (std::getline(list, one, ',')) {
        if (!one.empty()) out.push_back(one);
    }
    return out;
}

// Spoken characters: CJK ideographs and ASCII letters and digits. Punctuation
// takes no time to say, and counting it would make every rate a little wrong.
std::size_t spoken_chars(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ++n;
            ++i;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            const unsigned cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                                (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            if (cp >= 0x4E00 && cp <= 0x9FFF) ++n;
            i += 3;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
        } else if ((c & 0xF8) == 0xF0) {
            i += 4;
        } else {
            ++i;
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

// Mono 16-bit at `rate`: the one format Qtts::set_reference accepts.
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

bool read_spk(const std::string& path, std::vector<float>* out) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        return false;
    }
    std::stringstream all;
    all << in.rdbuf();
    const std::string bytes = all.str();
    if (bytes.empty() || bytes.size() % sizeof(float) != 0) {
        return false;
    }
    out->resize(bytes.size() / sizeof(float));
    std::copy(bytes.begin(), bytes.end(), reinterpret_cast<char*>(&(*out)[0]));
    return true;
}

bool write_f32(const std::string& path, const std::vector<float>& v) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = std::fwrite(&v[0], sizeof(float), v.size(), f) == v.size();
    std::fclose(f);
    return ok;
}

struct Measure {
    bool   ok       = false;
    double cps      = 0.0;
    double f0_st    = 0.0;
    double range_st = 0.0;
    double level_db = 0.0;
    double pause    = 0.0;
};

// One take, measured over the span between its first and last sound. Silence
// is the prosody module's own rule -- a fraction of this take's loudest frame --
// so a take that is quieter overall is not read as one with more pauses.
Measure measure(const std::vector<float>& pcm, int rate, std::size_t chars) {
    Measure m;
    yoai::voice::ProsodyOptions opt;
    const std::size_t hop = static_cast<std::size_t>(opt.frame_shift * rate);
    if (pcm.size() < hop * 4 || chars == 0) {
        return m;
    }
    std::vector<float> rms;
    for (std::size_t at = 0; at + hop <= pcm.size(); at += hop) {
        double sum = 0.0;
        for (std::size_t k = at; k < at + hop; ++k) sum += static_cast<double>(pcm[k]) * pcm[k];
        rms.push_back(static_cast<float>(std::sqrt(sum / hop)));
    }
    const float peak  = *std::max_element(rms.begin(), rms.end());
    const float floor = peak * opt.silence_fraction;
    std::size_t first = rms.size(), last = 0;
    for (std::size_t i = 0; i < rms.size(); ++i) {
        if (rms[i] > floor) {
            first = std::min(first, i);
            last  = i;
        }
    }
    if (first >= rms.size()) {
        return m;
    }
    const float t0 = static_cast<float>(first * hop) / rate;
    const float t1 = static_cast<float>((last + 1) * hop) / rate;

    yoai::voice::Recording rec;
    std::vector<std::pair<float, float> > span(1, std::make_pair(t0, t1));
    if (!yoai::voice::analyze_segments(&pcm[0], pcm.size(), rate, span, opt, &rec) ||
        rec.utterances.empty() || rec.utterances[0].f0_mean <= 0.0f) {
        return m;
    }
    const yoai::voice::Utterance& u = rec.utterances[0];
    m.ok       = true;
    m.cps      = static_cast<double>(chars) / (t1 - t0);
    m.f0_st    = 12.0 * std::log2(u.f0_mean / 100.0);
    m.range_st = u.f0_range_st;
    m.level_db = 20.0 * std::log10(std::max(1e-9f, u.rms_mean));
    m.pause    = 1.0 - u.speech_ratio;
    return m;
}

double cosine(const std::vector<float>& a, const std::vector<float>& b) {
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        ab += static_cast<double>(a[i]) * b[i];
        aa += static_cast<double>(a[i]) * a[i];
        bb += static_cast<double>(b[i]) * b[i];
    }
    return (aa > 0.0 && bb > 0.0) ? ab / std::sqrt(aa * bb) : 0.0;
}

double norm(const std::vector<float>& a) {
    double aa = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) aa += static_cast<double>(a[i]) * a[i];
    return std::sqrt(aa);
}

// One condition's takes: their mean, and how far apart they landed, which is
// the yardstick a style's movement is read against.
struct Cell {
    Measure mean;
    double  cps_spread = 0.0;
    double  f0_spread  = 0.0;
    int     takes      = 0;
};

Cell summarize(const std::vector<Measure>& takes) {
    Cell c;
    double cps_lo = 1e9, cps_hi = -1e9, f0_lo = 1e9, f0_hi = -1e9;
    for (std::size_t i = 0; i < takes.size(); ++i) {
        const Measure& t = takes[i];
        if (!t.ok) continue;
        c.mean.cps += t.cps;
        c.mean.f0_st += t.f0_st;
        c.mean.range_st += t.range_st;
        c.mean.level_db += t.level_db;
        c.mean.pause += t.pause;
        cps_lo = std::min(cps_lo, t.cps);
        cps_hi = std::max(cps_hi, t.cps);
        f0_lo  = std::min(f0_lo, t.f0_st);
        f0_hi  = std::max(f0_hi, t.f0_st);
        ++c.takes;
    }
    if (c.takes > 0) {
        c.mean.ok = true;
        c.mean.cps /= c.takes;
        c.mean.f0_st /= c.takes;
        c.mean.range_st /= c.takes;
        c.mean.level_db /= c.takes;
        c.mean.pause /= c.takes;
        c.cps_spread = cps_hi - cps_lo;
        c.f0_spread  = f0_hi - f0_lo;
    }
    return c;
}

void print_header(const std::string& title) {
    std::printf("\n%s\n", title.c_str());
    std::printf("%-9s %5s %6s %6s %7s %6s %6s %s\n", "style", "c/s", "F0 st", "range", "level", "pause",
                "RTF", "takes");
}

void print_row(const std::string& name, const Cell& c, double rtf) {
    if (!c.mean.ok) {
        std::printf("%-9s failed\n", name.c_str());
        return;
    }
    std::printf("%-9s %5.2f %6.1f %6.1f %7.1f %6.2f %6.2f %d", name.c_str(), c.mean.cps, c.mean.f0_st,
                c.mean.range_st, c.mean.level_db, c.mean.pause, rtf, c.takes);
    if (c.takes > 1) {
        std::printf("  (apart: c/s %.2f, F0 %.1f st)", c.cps_spread, c.f0_spread);
    }
    std::printf("\n");
}

// Base reading the test passage once per seed, under whatever reference the
// engine holds now. Writes each take as <prefix>-<seed>.wav.
Cell read_test(Qtts* tts, const std::vector<long long>& seeds, const std::string& prefix,
               double* elapsed, double* speech) {
    const std::size_t chars = spoken_chars(kTestText);
    std::vector<Measure> takes;
    for (std::size_t s = 0; s < seeds.size(); ++s) {
        tts->set_seed(seeds[s]);
        std::vector<float> pcm;
        QttsResult r;
        if (!tts->say(kTestText, &collect, &pcm, &r) || pcm.empty()) {
            std::fprintf(stderr, "%s seed %lld: %s\n", prefix.c_str(), seeds[s], tts->last_error().c_str());
            continue;
        }
        *elapsed += r.elapsed;
        *speech += r.speech_seconds;
        char name[32];
        std::snprintf(name, sizeof(name), "-%lld.wav", seeds[s]);
        write_wav(prefix + name, pcm, Qtts::sample_rate());
        takes.push_back(measure(pcm, Qtts::sample_rate(), chars));
    }
    return summarize(takes);
}

// A column of a change-from-baseline table: its name, and one cell per style
// with the baseline at index 0.
struct Column {
    std::string       name;
    std::vector<Cell> cells;
};

void print_delta_table(const char* title, const std::vector<int>& styles, const std::vector<Column>& cols,
                       double Measure::*field, const char* unit) {
    std::printf("\n%s, change from calm (%s)\n", title, unit);
    std::printf("%-9s", "style");
    for (std::size_t c = 0; c < cols.size(); ++c) std::printf(" %8s", cols[c].name.c_str());
    std::printf("\n");
    for (std::size_t i = 1; i < styles.size(); ++i) {
        std::printf("%-9s", kStyles[styles[i]].name);
        for (std::size_t c = 0; c < cols.size(); ++c) {
            const Cell& base = cols[c].cells[0];
            const Cell& cell = cols[c].cells[i];
            if (base.mean.ok && cell.mean.ok) {
                std::printf(" %+8.2f", cell.mean.*field - base.mean.*field);
            } else {
                std::printf(" %8s", "-");
            }
        }
        std::printf("\n");
    }
}

// One reference take: who read it, at which seed, in which style.
struct Reference {
    std::string        speaker;
    long long          seed  = 0;
    std::size_t        style = 0;   // index into the run's styles
    std::string        wav;
    Measure            measure;
    std::vector<float> vector;      // its voice file, once Base has extracted it
};

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> speakers(1, "serena");
    std::vector<long long>   ref_seeds(1, 101);
    std::vector<long long>   seeds;
    seeds.push_back(23);
    seeds.push_back(101);
    seeds.push_back(202);
    std::vector<int>         styles;
    std::vector<double>      alphas(1, 1.0);
    std::string              own_path;
    std::string              directions_dir;
    bool                     ab            = true;
    bool                     refs_only     = false;
    bool                     styles_given  = false;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--speakers" && i + 1 < argc) {
            speakers = split_list(argv[++i]);
        } else if (a == "--ref-seeds" && i + 1 < argc) {
            ref_seeds.clear();
            const std::vector<std::string> list = split_list(argv[++i]);
            for (std::size_t k = 0; k < list.size(); ++k) ref_seeds.push_back(std::atoll(list[k].c_str()));
        } else if (a == "--seeds" && i + 1 < argc) {
            seeds.clear();
            const std::vector<std::string> list = split_list(argv[++i]);
            for (std::size_t k = 0; k < list.size(); ++k) seeds.push_back(std::atoll(list[k].c_str()));
        } else if (a == "--styles" && i + 1 < argc) {
            styles_given = true;
            const std::vector<std::string> list = split_list(argv[++i]);
            for (std::size_t k = 0; k < list.size(); ++k) {
                int found = -1;
                for (int s = 0; s < kStyleCount; ++s) {
                    if (list[k] == kStyles[s].name) found = s;
                }
                if (found < 0) {
                    std::fprintf(stderr, "no style named %s\n", list[k].c_str());
                    return 2;
                }
                if (found > 0 && std::find(styles.begin(), styles.end(), found) == styles.end()) {
                    styles.push_back(found);
                }
            }
        } else if (a == "--alphas" && i + 1 < argc) {
            alphas.clear();
            const std::vector<std::string> list = split_list(argv[++i]);
            for (std::size_t k = 0; k < list.size(); ++k) alphas.push_back(std::atof(list[k].c_str()));
        } else if (a == "--own" && i + 1 < argc) {
            own_path = argv[++i];
        } else if (a == "--no-ab") {
            ab = false;
        } else if (a == "--refs-only") {
            refs_only = true;
        } else if (a == "--directions" && i + 1 < argc) {
            directions_dir = argv[++i];
        } else {
            args.push_back(a);
        }
    }
    if (styles.empty() && !styles_given) {
        for (int s = 1; s < kStyleCount; ++s) styles.push_back(s);
    }
    styles.insert(styles.begin(), 0);   // calm, the baseline, first
    if (styles.size() < 2 && !refs_only) {
        std::fprintf(stderr, "calm alone gives nothing to compare; it is for --refs-only\n");
        return 2;
    }
    if (!directions_dir.empty() && own_path.empty()) {
        std::fprintf(stderr, "--directions needs --own\n");
        return 2;
    }
    if (args.size() != 4 || speakers.empty() || ref_seeds.empty() || seeds.empty() || alphas.empty()) {
        std::fprintf(stderr,
                     "usage: yoai-tts-style [--speakers <a,b>] [--ref-seeds <a,b>] [--seeds <a,b>] "
                     "[--styles <a,b>] [--own <voice.spk>] [--alphas <a,b>] [--no-ab] [--refs-only] "
                     "[--directions <dir>] <customvoice.gguf> <base.gguf> <tokenizer.gguf> <out_dir>\n");
        return 2;
    }
    const std::string out = args[3] + "/";
    if (!Qtts::available()) {
        std::fprintf(stderr, "this build has no speech engine\n");
        return 1;
    }
    const int rate = Qtts::sample_rate();
    const std::size_t n_styles = styles.size();

    // References: every speaker, every seed, every style. None when the
    // directions come from a file.
    std::vector<Reference> refs;
    if (directions_dir.empty()) {
        Qtts tts;
        if (!tts.load(args[0], args[2])) {
            std::fprintf(stderr, "load %s: %s\n", args[0].c_str(), tts.last_error().c_str());
            return 1;
        }
        if (tts.model_type() != "custom_voice") {
            std::fprintf(stderr, "%s is %s, not custom_voice\n", args[0].c_str(), tts.model_type().c_str());
            return 1;
        }
        const std::vector<std::string> names = tts.speakers();
        std::printf("CustomVoice speakers:");
        for (std::size_t k = 0; k < names.size(); ++k) std::printf(" %s", names[k].c_str());
        std::printf("\n");
        for (std::size_t k = 0; k < speakers.size(); ++k) {
            if (std::find(names.begin(), names.end(), speakers[k]) == names.end()) {
                std::fprintf(stderr, "no speaker named %s in %s\n", speakers[k].c_str(), args[0].c_str());
                return 1;
            }
        }
        apply_reading(&tts);
        const std::size_t chars = spoken_chars(kReferenceText);
        for (std::size_t k = 0; k < speakers.size(); ++k) {
            tts.set_speaker(speakers[k]);
            for (std::size_t s = 0; s < ref_seeds.size(); ++s) {
                tts.set_seed(ref_seeds[s]);
                char title[96];
                std::snprintf(title, sizeof(title), "references: CustomVoice %s, seed %lld", speakers[k].c_str(),
                              ref_seeds[s]);
                print_header(title);
                for (std::size_t i = 0; i < n_styles; ++i) {
                    Reference ref;
                    ref.speaker = speakers[k];
                    ref.seed    = ref_seeds[s];
                    ref.style   = i;
                    char name[160];
                    std::snprintf(name, sizeof(name), "ref-%s-%lld-%s.wav", speakers[k].c_str(), ref_seeds[s],
                                  kStyles[styles[i]].name);
                    ref.wav = out + name;
                    tts.set_voice(kStyles[styles[i]].instruct);
                    std::vector<float> pcm;
                    QttsResult r;
                    if (!tts.say(kReferenceText, &collect, &pcm, &r) || pcm.empty() ||
                        !write_wav(ref.wav, pcm, rate)) {
                        std::fprintf(stderr, "reference %s: %s\n", name, tts.last_error().c_str());
                        return 1;
                    }
                    ref.measure = measure(pcm, rate, chars);
                    std::vector<Measure> one(1, ref.measure);
                    print_row(kStyles[styles[i]].name, summarize(one),
                              r.speech_seconds > 0 ? r.elapsed / r.speech_seconds : 0.0);
                    refs.push_back(ref);
                }
            }
        }
    }

    // The reference column of every delta table: each style's takes over all
    // speakers and seeds. Its change from calm is the mean of the per-pair
    // changes, since every pair contributes one calm and one of each style.
    std::vector<Cell> ref_cells;
    for (std::size_t i = 0; i < n_styles; ++i) {
        std::vector<Measure> takes;
        for (std::size_t k = 0; k < refs.size(); ++k) {
            if (refs[k].style == i) takes.push_back(refs[k].measure);
        }
        ref_cells.push_back(summarize(takes));
    }

    Qtts tts;
    if (!tts.load(args[1], args[2])) {
        std::fprintf(stderr, "load %s: %s\n", args[1].c_str(), tts.last_error().c_str());
        return 1;
    }
    if (tts.model_type() != "base") {
        std::fprintf(stderr, "%s is %s, not base\n", args[1].c_str(), tts.model_type().c_str());
        return 1;
    }
    apply_reading(&tts);

    for (std::size_t k = 0; k < refs.size(); ++k) {
        if (!tts.set_reference_voice(refs[k].wav)) {
            std::fprintf(stderr, "extract %s: %s\n", refs[k].wav.c_str(), tts.last_error().c_str());
            return 1;
        }
        std::string spk = refs[k].wav;
        spk.replace(spk.size() - 4, 4, ".spk");
        tts.save_reference_embedding(spk);
        refs[k].vector = tts.reference_embedding();
    }
    if (refs_only) {
        std::printf("\n%d voice files written beside their references in %s\n", static_cast<int>(refs.size()),
                    args[3].c_str());
        return 0;
    }

    // Directions. A pair is one speaker at one seed; its calm is the baseline
    // for its styles, so who is speaking cancels inside the pair.
    std::vector<std::vector<float> > directions(n_styles);
    if (!directions_dir.empty()) {
        std::printf("\ndirections: from %s\n", directions_dir.c_str());
        std::printf("%-9s %8s\n", "style", "norm");
        for (std::size_t i = 1; i < n_styles; ++i) {
            const std::string path = directions_dir + "/direction-" + kStyles[styles[i]].name + ".f32";
            if (!read_spk(path, &directions[i])) {
                std::fprintf(stderr, "cannot read %s\n", path.c_str());
                return 1;
            }
            std::printf("%-9s %8.3f\n", kStyles[styles[i]].name, norm(directions[i]));
        }
    } else {
        std::printf("\ndirections: mean over %d speaker-seed pairs of emb(style) - emb(calm)\n",
                    static_cast<int>(speakers.size() * ref_seeds.size()));
        std::printf("%-9s %8s %8s %8s\n", "style", "norm", "single", "agree");
    }
    for (std::size_t i = 1; i < n_styles && directions_dir.empty(); ++i) {
        std::vector<std::vector<float> > diffs;
        for (std::size_t k = 0; k < refs.size(); ++k) {
            if (refs[k].style != i) continue;
            for (std::size_t c = 0; c < refs.size(); ++c) {
                if (refs[c].style == 0 && refs[c].speaker == refs[k].speaker && refs[c].seed == refs[k].seed) {
                    std::vector<float> d = refs[k].vector;
                    for (std::size_t j = 0; j < d.size(); ++j) d[j] -= refs[c].vector[j];
                    diffs.push_back(d);
                }
            }
        }
        std::vector<float> mean(diffs[0].size(), 0.0f);
        double single = 0.0;
        for (std::size_t p = 0; p < diffs.size(); ++p) {
            for (std::size_t j = 0; j < mean.size(); ++j) mean[j] += diffs[p][j] / diffs.size();
            single += norm(diffs[p]) / diffs.size();
        }
        // The mean cosine between pairs: 1 when every pair moved the same way,
        // near 0 when what moved was each take's own noise.
        double agree = 0.0;
        int    pairs = 0;
        for (std::size_t p = 0; p < diffs.size(); ++p) {
            for (std::size_t q = p + 1; q < diffs.size(); ++q) {
                agree += cosine(diffs[p], diffs[q]);
                ++pairs;
            }
        }
        directions[i] = mean;
        write_f32(out + "direction-" + kStyles[styles[i]].name + ".f32", mean);
        if (pairs > 0) {
            std::printf("%-9s %8.3f %8.3f %8.3f\n", kStyles[styles[i]].name, norm(mean), single, agree / pairs);
        } else {
            std::printf("%-9s %8.3f %8.3f %8s\n", kStyles[styles[i]].name, norm(mean), single, "-");
        }
    }

    std::vector<Column> cols;
    if (!refs.empty()) {
        cols.push_back(Column());
        cols.back().name  = "ref";
        cols.back().cells = ref_cells;
    }
    double elapsed = 0.0, speech = 0.0;

    if (ab && !refs.empty()) {
        // The first speaker's first seed, as one reference per style.
        std::vector<const Reference*> first(n_styles, static_cast<const Reference*>(0));
        for (std::size_t k = 0; k < refs.size(); ++k) {
            if (refs[k].speaker == speakers[0] && refs[k].seed == ref_seeds[0]) first[refs[k].style] = &refs[k];
        }
        Column a, b;
        a.name = "A";
        b.name = "B";
        print_header("mode A: Base, each reference as a voice file");
        for (std::size_t i = 0; i < n_styles; ++i) {
            tts.set_reference_embedding(first[i]->vector);
            elapsed = speech = 0.0;
            a.cells.push_back(read_test(&tts, seeds, out + "A-" + kStyles[styles[i]].name, &elapsed, &speech));
            print_row(kStyles[styles[i]].name, a.cells.back(), speech > 0 ? elapsed / speech : 0.0);
        }
        print_header("mode B: Base, each reference and its transcript in the prompt");
        for (std::size_t i = 0; i < n_styles; ++i) {
            if (!tts.set_reference(first[i]->wav, kReferenceText)) {
                std::fprintf(stderr, "reference %s: %s\n", first[i]->wav.c_str(), tts.last_error().c_str());
                return 1;
            }
            elapsed = speech = 0.0;
            b.cells.push_back(read_test(&tts, seeds, out + "B-" + kStyles[styles[i]].name, &elapsed, &speech));
            print_row(kStyles[styles[i]].name, b.cells.back(), speech > 0 ? elapsed / speech : 0.0);
        }
        cols.push_back(a);
        cols.push_back(b);
    }

    if (!own_path.empty()) {
        std::vector<float> own;
        if (!read_spk(own_path, &own) || own.size() != directions[1].size()) {
            std::fprintf(stderr, "--own %s: not a voice file for this talker\n", own_path.c_str());
            return 1;
        }
        // The voice file alone is every alpha's baseline, read once.
        tts.set_reference_embedding(own);
        elapsed = speech = 0.0;
        const Cell base = read_test(&tts, seeds, out + "own-calm", &elapsed, &speech);
        print_header("own: the --own voice file alone");
        print_row("own", base, speech > 0 ? elapsed / speech : 0.0);
        for (std::size_t a = 0; a < alphas.size(); ++a) {
            char tag[32];
            std::snprintf(tag, sizeof(tag), "a%g", alphas[a]);
            Column col;
            col.name = std::string("own ") + tag;
            col.cells.push_back(base);
            print_header("own + " + std::string(tag + 1) + " x direction");
            for (std::size_t i = 1; i < n_styles; ++i) {
                std::vector<float> v = own;
                for (std::size_t j = 0; j < v.size(); ++j) v[j] += static_cast<float>(alphas[a]) * directions[i][j];
                tts.set_reference_embedding(v);
                elapsed = speech = 0.0;
                col.cells.push_back(read_test(&tts, seeds,
                                              out + "own-" + tag + "-" + kStyles[styles[i]].name, &elapsed, &speech));
                print_row(kStyles[styles[i]].name, col.cells.back(), speech > 0 ? elapsed / speech : 0.0);
            }
            cols.push_back(col);
        }
    }

    print_delta_table("pace", styles, cols, &Measure::cps, "characters a second");
    print_delta_table("pitch", styles, cols, &Measure::f0_st, "semitones");
    print_delta_table("pitch range", styles, cols, &Measure::range_st, "semitones");
    print_delta_table("level", styles, cols, &Measure::level_db, "dB");
    std::printf("\nWAVs in %s\n", args[3].c_str());
    return 0;
}
