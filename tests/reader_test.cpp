// The sentence splitter, checked against the failures that produced it.
//
//     build/yoai-reader-test
//     build/yoai-reader-test <talker.gguf> <tokenizer.gguf> [voices] [instruct] [seed]
//                            [subtalker_temperature] [reference.wav|.spk] [min_chars]
//
// With no arguments this is split_sentences() and SentenceStream alone --
// plain text processing, so it runs anywhere the core builds and says yes or no
// on its own.
//
// Given the two GGUFs it also reads a passage aloud on `voices` engines and
// checks the half that cannot be checked without one: that the audio arrives in
// reading order, that the end marker arrives exactly once and last, and that
// nothing follows it. It writes reader.wav and prints the timings, which is how
// the question Reader exists to answer gets asked -- run it at 1 voice and at
// 2 and compare `elapsed` for the same passage.
//
// **The invariant is the point.** An earlier version of this splitter lived
// inline in the desktop client and lost text: it advanced a cursor past
// sentences it judged too short, and "你好。" was silently never spoken. That is
// the kind of defect nobody reports as a bug -- the reply is read aloud, it
// just does not say everything -- so every case here re-checks that the pieces
// cover the passage exactly, and the short-sentence cases are the ones that
// caught it.

#include "voice/prosody/prosody.hpp"
#include "voice/text/sentence_stream.hpp"
#include "voice/text/speakable.hpp"
#include "voice/tts/reader.hpp"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using yoai::voice::Reader;
using yoai::voice::ReaderResult;
using yoai::voice::Sentence;
using yoai::voice::SentenceStream;
using yoai::voice::SplitOptions;
using yoai::voice::split_sentences;

namespace {

int g_failures = 0;

void fail(const char* what, const std::string& detail) {
    std::printf("  FAIL %s: %s\n", what, detail.c_str());
    ++g_failures;
}

void expect(bool ok, const char* what, const std::string& detail) {
    if (!ok) {
        fail(what, detail);
    }
}

// Characters, not bytes -- the same count the options are expressed in.
std::size_t chars_of(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0)      len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) len = 1;
        i += len;
        ++n;
    }
    return n;
}

// Every case runs through here, whatever else it checks.
std::vector<Sentence> split_checked(const std::string& text, const SplitOptions& opt,
                                    const char* label) {
    const std::vector<Sentence> out = split_sentences(text, opt);

    std::string joined;
    std::size_t expect_begin = 0;
    for (std::size_t k = 0; k < out.size(); ++k) {
        if (out[k].begin != expect_begin) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "piece %d starts at %d, previous ended at %d",
                          static_cast<int>(k), static_cast<int>(out[k].begin),
                          static_cast<int>(expect_begin));
            fail(label, buf);
        }
        if (out[k].end < out[k].begin || out[k].end > text.size()) {
            fail(label, "piece range is out of bounds");
        }
        if (out[k].text != text.substr(out[k].begin, out[k].end - out[k].begin)) {
            fail(label, "piece text does not match its own range");
        }
        joined += out[k].text;
        expect_begin = out[k].end;
    }
    if (expect_begin != text.size()) {
        fail(label, "the pieces do not reach the end of the passage");
    }
    if (joined != text) {
        fail(label, "concatenating the pieces does not reproduce the passage");
    }
    return out;
}

void show(const char* label, const std::vector<Sentence>& out) {
    std::printf("%s -> %d piece(s)\n", label, static_cast<int>(out.size()));
    for (std::size_t k = 0; k < out.size(); ++k) {
        std::printf("    [%d] %2d chars  %s\n", static_cast<int>(k),
                    static_cast<int>(chars_of(out[k].text)), out[k].text.c_str());
    }
}

//------------------------------------------------------------------------------
// Markdown to speech
//------------------------------------------------------------------------------

// `markdown` must come out as `spoken`, whole AND fed in pieces of every size
// from one byte up -- the pieces cut through UTF-8 sequences, bullets and line
// breaks, which is exactly what the desktop version got wrong.
void speakable_case(const char* label, const std::string& markdown, const std::string& spoken) {
    const std::string whole = yoai::voice::speakable(markdown);
    std::printf("speakable %-10s -> %s\n", label, whole.c_str());
    expect(whole == spoken, label, "expected: " + spoken);

    const std::size_t steps[] = {1, 2, 3, 5, 7, 13};
    for (std::size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); ++s) {
        yoai::voice::SpeakableStream stream;
        std::string got;
        for (std::size_t i = 0; i < markdown.size(); i += steps[s]) {
            got += stream.feed(markdown.substr(i, steps[s]));
        }
        got += stream.finish();
        if (got != whole) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "fed %d byte(s) at a time: ",
                          static_cast<int>(steps[s]));
            fail(label, buf + got);
        }
    }
}

void check_speakable() {
    // The reply that found it: a list whose items carry no punctuation, where
    // the line breaks were the only thing between "上海" and "南接".
    speakable_case("list",
        "太湖位于中国江苏省南部和浙江省北部。具体地理范围包括：\n"
        "- 东邻：上海\n- 南接：浙江湖州、嘉兴等地\n- 西连：安徽芜湖\n"
        "- 北靠：江苏南京、镇江及苏州、无锡、常州等市\n\n"
        "它是我国第三大淡水湖，也是长江下游南岸最大的湖泊。",
        "太湖位于中国江苏省南部和浙江省北部。具体地理范围包括。东邻：上海。"
        "南接：浙江湖州、嘉兴等地。西连：安徽芜湖。"
        "北靠：江苏南京、镇江及苏州、无锡、常州等市。"
        "它是我国第三大淡水湖，也是长江下游南岸最大的湖泊。");
    speakable_case("english",
        "Two points:\n- first line\n- second line\n\nDone.",
        "Two points. first line. second line. Done.");
    // Single "_" and "~" are words; doubled ones are markup.
    speakable_case("inline",
        "**榨菜**是一种`下饭菜`，Q4_K_M 很小，3~5 度，~~删掉~~保留，__粗__体。",
        "榨菜是一种下饭菜，Q4_K_M 很小，3~5 度，删掉保留，粗体。");
    speakable_case("links",
        "看[文档](docs/a_b.md)和![图](x.png)，[没有链接]就算了。",
        "看文档和图，[没有链接]就算了。");
    speakable_case("table",
        "| 城市 | 人口 |\n|---|---|\n| 上海 | 2400万 |\n",
        "城市，人口。上海，2400万。");
    speakable_case("fence",
        "看这段：\n```python\nx = 1\n```\n就这样",
        "看这段。x = 1. 就这样");
    speakable_case("blocks",
        "# 标题\n> 引用一句\n---\n正文",
        "标题。引用一句。正文");
    // A bullet needs a space after it; these are words that start with a mark.
    speakable_case("not marks",
        "温度：\n-5 度\n#话题 也算\n*斜体*开头",
        "温度。-5 度。#话题 也算。斜体开头");
    speakable_case("emoji",
        "好的\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBB，我们走吧\xF0\x9F\x9A\x80！"
        "1\xEF\xB8\x8F\xE2\x83\xA3 第一",
        "好的，我们走吧！1 第一");
    speakable_case("closers",
        "他说：“好。”\n（上海）\n下一行",
        "他说：“好。”（上海）。下一行");
    // A line already ending in a full stop gets no second one: two in a row
    // are read as a hesitation, an audible "呃". A comma before the break is
    // replaced; one inside the line stays.
    speakable_case("stops",
        "第一句说完了。\n第二句，还没完，\n第三句！\nLast one.\nand, then\n结束",
        "第一句说完了。第二句，还没完。第三句！Last one. and, then. 结束");
    speakable_case("prose",
        "你好！有什么我可以帮你的吗？",
        "你好！有什么我可以帮你的吗？");
    speakable_case("only marks", "---\n```\n\n", "");
}

//------------------------------------------------------------------------------
// The live half
//------------------------------------------------------------------------------

// What the sink saw, in the order it saw it. The ordering guarantee is the
// whole reason Reader is more than a loop over say(), so it is checked rather
// than assumed.
struct Heard {
    std::vector<float> pcm;
    int  last_sentence;
    int  calls;
    int  last_markers;
    bool out_of_order;
    bool after_last;
    std::chrono::steady_clock::time_point started;

    // Where each piece begins in `pcm`, in samples.
    //
    // The pause cut cannot answer the question that matters here -- "is it ONE
    // piece that came out wrong" -- because its units land wherever the speaker
    // breathed, which is nearly but not exactly at the cuts. These are the cuts
    // themselves, and they only exist because the sink is told which sentence
    // every handover belongs to.
    std::vector<std::size_t> piece_start;

    Heard() : last_sentence(-1), calls(0), last_markers(0),
              out_of_order(false), after_last(false),
              started(std::chrono::steady_clock::now()) {}

    double at() const {
        return std::chrono::duration_cast<std::chrono::duration<double> >(
                   std::chrono::steady_clock::now() - started).count();
    }
};

bool on_audio(const float* pcm, std::size_t n, int sentence, bool last, void* user_data) {
    Heard* h = static_cast<Heard*>(user_data);
    ++h->calls;
    if (h->last_markers > 0) {
        h->after_last = true;   // anything at all after the end marker is a bug
    }
    if (last) {
        ++h->last_markers;
        return true;
    }
    if (sentence < h->last_sentence) {
        h->out_of_order = true;
    }
    if (sentence != h->last_sentence) {
        h->piece_start.push_back(h->pcm.size());
    }
    h->last_sentence = sentence;
    h->pcm.insert(h->pcm.end(), pcm, pcm + n);
    return true;
}

void put32(std::FILE* f, unsigned v) {
    const unsigned char b[4] = {static_cast<unsigned char>(v & 0xFF),
                                static_cast<unsigned char>((v >> 8) & 0xFF),
                                static_cast<unsigned char>((v >> 16) & 0xFF),
                                static_cast<unsigned char>((v >> 24) & 0xFF)};
    std::fwrite(b, 1, 4, f);
}

void put16(std::FILE* f, unsigned v) {
    const unsigned char b[2] = {static_cast<unsigned char>(v & 0xFF),
                                static_cast<unsigned char>((v >> 8) & 0xFF)};
    std::fwrite(b, 1, 2, f);
}

// 16-bit mono, at the rate the engine produces. Written so the result can be
// listened to, which is the only check that matters for the seams.
bool write_wav(const char* path, const std::vector<float>& pcm, int rate) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        return false;
    }
    const unsigned bytes = static_cast<unsigned>(pcm.size() * 2);
    std::fwrite("RIFF", 1, 4, f);
    put32(f, 36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);                                  // PCM
    put16(f, 1);                                  // mono
    put32(f, static_cast<unsigned>(rate));
    put32(f, static_cast<unsigned>(rate) * 2);    // byte rate
    put16(f, 2);                                  // block align
    put16(f, 16);                                 // bits
    std::fwrite("data", 1, 4, f);
    put32(f, bytes);
    for (std::size_t k = 0; k < pcm.size(); ++k) {
        float s = pcm[k];
        if (s > 1.0f)  s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        put16(f, static_cast<unsigned short>(static_cast<short>(s * 32767.0f)));
    }
    std::fclose(f);
    return true;
}

// The passage fed in a token at a time, the way the chat lane produces it.
//
// This is the entry point the class exists for, and what it has to prove is not
// the audio -- read() already covers that -- but that the answer is the SAME
// whichever door the text comes through. So it runs the identical passage
// through begin/feed/finish and compares.
//
// `tok_chars` is the size of a delta. A language model emits one to two
// characters per token, and anything from 1 upwards should behave identically:
// a piece is dispatched only once a terminator has settled it.
bool streamed_pass(Reader& reader, const std::string& passage, const SplitOptions& opt,
                   std::size_t tok_chars, std::vector<float>* pcm, ReaderResult* out,
                   int* failures) {
    reader.set_split(opt);

    Heard heard;
    heard.started = std::chrono::steady_clock::now();
    if (!reader.begin(&on_audio, &heard)) {
        std::printf("  FAIL stream: %s\n", reader.last_error().c_str());
        ++*failures;
        return false;
    }

    // Cut on CHARACTER boundaries, not byte ones: half a UTF-8 sequence is not
    // text, and handing one over would be a bug in this harness rather than a
    // test of the class.
    std::size_t i = 0, since = 0, start = 0;
    while (i < passage.size()) {
        const unsigned char c = static_cast<unsigned char>(passage[i]);
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0)      len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > passage.size()) len = 1;
        i += len;
        if (++since >= tok_chars) {
            reader.feed(passage.substr(start, i - start));
            start = i;
            since = 0;
        }
    }
    if (start < passage.size()) {
        reader.feed(passage.substr(start));
    }

    if (!reader.finish(out)) {
        std::printf("  FAIL stream: %s\n", reader.last_error().c_str());
        ++*failures;
        return false;
    }
    if (heard.out_of_order) {
        std::printf("  FAIL stream: audio arrived out of reading order\n");
        ++*failures;
    }
    if (heard.last_markers != 1 || heard.after_last) {
        std::printf("  FAIL stream: the end marker is wrong (%d of them)\n",
                    heard.last_markers);
        ++*failures;
    }
    pcm->swap(heard.pcm);
    return true;
}

// The passage written at a steady pace, one character every `ms_per_char`,
// the way a language model writes it -- for the pacing, which only exists
// against a clock. Prints what was cut; the reader logs each piece as well.
bool paced_pass(Reader& reader, const std::string& passage, const SplitOptions& opt,
                int ms_per_char, ReaderResult* out, int* failures) {
    reader.set_split(opt);
    Heard heard;
    if (!reader.begin(&on_audio, &heard)) {
        std::printf("  FAIL paced: %s\n", reader.last_error().c_str());
        ++*failures;
        return false;
    }
    for (std::size_t i = 0; i < passage.size(); ) {
        const unsigned char c = static_cast<unsigned char>(passage[i]);
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0)      len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        reader.feed(passage.substr(i, len));
        i += len;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms_per_char));
    }
    if (!reader.finish(out)) {
        std::printf("  FAIL paced: %s\n", reader.last_error().c_str());
        ++*failures;
        return false;
    }
    if (heard.out_of_order || heard.last_markers != 1 || heard.after_last) {
        std::printf("  FAIL paced: order or end marker wrong\n");
        ++*failures;
    }
    return true;
}

// One pass over the passage, with the splitting the caller asked for.
//
// Returns the audio, or leaves `failures` raised. The Reader is reused between
// passes on purpose: the comparison this is for has to hold everything except
// the cutting fixed, and reloading between them would reintroduce the cold-run
// difference that made the first measurement of this lane read 1.30x when it
// was 0.57x.
bool one_pass(Reader& reader, const std::string& passage, const SplitOptions& opt,
              const char* label, std::vector<float>* pcm, std::vector<std::size_t>* starts,
              ReaderResult* out, int* failures) {
    reader.set_split(opt);

    Heard heard;
    heard.started = std::chrono::steady_clock::now();
    if (!reader.read(passage, &on_audio, &heard, out)) {
        std::printf("  FAIL %s: %s\n", label, reader.last_error().c_str());
        ++*failures;
        return false;
    }
    if (heard.out_of_order) {
        std::printf("  FAIL %s: audio arrived out of reading order\n", label);
        ++*failures;
    }
    if (heard.last_markers != 1 || heard.after_last) {
        std::printf("  FAIL %s: the end marker is wrong (%d of them)\n",
                    label, heard.last_markers);
        ++*failures;
    }
    const std::size_t expect_samples =
        static_cast<std::size_t>(out->speech_seconds * Reader::sample_rate() + 0.5f);
    if (heard.pcm.size() != expect_samples) {
        std::printf("  FAIL %s: %d samples heard, %d reported\n", label,
                    static_cast<int>(heard.pcm.size()), static_cast<int>(expect_samples));
        ++*failures;
    }
    pcm->swap(heard.pcm);
    if (starts) {
        starts->swap(heard.piece_start);
    }
    return true;
}

// Each PIECE on its own, at the cuts the splitter actually made.
//
// This is the measurement the aggregate one missed. A whole take and a split
// take can have the same spread over their utterances while one of the split
// take's four sentences is in a different voice -- an average over six units
// does not care which unit it was, and a listener does. Here every row is one
// say(), so a row that sits apart from the others is a piece that came out
// wrong rather than a boundary that sounded rough.
void describe_pieces(const std::vector<float>& pcm, const std::vector<std::size_t>& starts,
                     const std::vector<Sentence>& cut) {
    if (starts.size() < 2) {
        return;
    }
    const double rate = Reader::sample_rate();
    std::vector<std::pair<float, float> > segments;
    for (std::size_t k = 0; k < starts.size(); ++k) {
        const std::size_t end = (k + 1 < starts.size()) ? starts[k + 1] : pcm.size();
        segments.push_back(std::make_pair(static_cast<float>(starts[k] / rate),
                                          static_cast<float>(end / rate)));
    }

    yoai::voice::Recording rec;
    yoai::voice::ProsodyOptions opt;
    if (!yoai::voice::analyze_segments(&pcm[0], pcm.size(), Reader::sample_rate(),
                                            segments, opt, &rec)) {
        std::printf("          per piece: too short to analyze\n");
        return;
    }
    // The middle to compare each row against. The mean would be dragged by the
    // outlier this exists to find, so the median is what the rows are read
    // against.
    std::vector<float> f0s;
    for (std::size_t k = 0; k < rec.utterances.size(); ++k) {
        if (rec.utterances[k].f0_mean > 0.0f) {
            f0s.push_back(rec.utterances[k].f0_mean);
        }
    }
    if (f0s.empty()) {
        return;
    }
    std::sort(f0s.begin(), f0s.end());
    const float mid = f0s[f0s.size() / 2];

    // Characters a second, per piece, which is what "the pace is too even"
    // means once it is written down. A whole take plans its pace over the
    // passage -- one sentence hurried, the next drawn out -- while pieces
    // generated independently each start from the same sentence-initial state,
    // so the spread between them collapses. The SPREAD is the reading, not the
    // value: 5 characters a second is neither good nor bad, and four pieces
    // within a tenth of each other is a recitation.
    double rate_min = 0.0, rate_max = 0.0;
    int rates = 0;

    std::printf("          per piece, against the median %.1f Hz:\n", mid);
    for (std::size_t k = 0; k < rec.utterances.size(); ++k) {
        const yoai::voice::Utterance& u = rec.utterances[k];
        const double st = (u.f0_mean > 0.0f)
                              ? 12.0 * std::log(u.f0_mean / mid) / std::log(2.0)
                              : 0.0;
        const double seconds = segments[k].second - segments[k].first;
        double cps = 0.0;
        if (k < cut.size() && seconds > 0.0) {
            cps = chars_of(cut[k].text) / seconds;
            if (rates == 0 || cps < rate_min) rate_min = cps;
            if (rates == 0 || cps > rate_max) rate_max = cps;
            ++rates;
        }
        std::printf("            piece %d  %5.2f-%5.2f s  F0 %6.1f (%+5.2f st)  RMS %.4f  "
                    "%.2f char/s\n",
                    static_cast<int>(k), segments[k].first, segments[k].second,
                    u.f0_mean, st, u.rms_mean, cps);
    }
    if (rates > 1) {
        std::printf("          pace %.2f-%.2f char/s, spread %.2f\n",
                    rate_min, rate_max, rate_max - rate_min);
    }
}

// The voice, measured the way design/voice.md measures a recording.
//
// The pause cut rather than the sentence boundaries, deliberately: it is the
// unit this file's arithmetic was validated on, and on a read passage the model
// pauses at the full stops anyway, so the units land on the sentences without
// being told where they are. Using the cuts as supplied would also beg the
// question -- it would measure the pieces this test made rather than the pieces
// a listener hears.
void describe(const char* label, const std::vector<float>& pcm) {
    yoai::voice::Recording rec;
    yoai::voice::ProsodyOptions opt;
    if (!yoai::voice::analyze(&pcm[0], pcm.size(), Reader::sample_rate(), opt, &rec)) {
        std::printf("  %-6s  too short to analyze\n", label);
        return;
    }
    // The STEP between neighbours, not the spread over the whole take.
    //
    // A seam is a jump at one instant, and a standard deviation over every
    // utterance counts a slow drift the same way -- it washes the jump out.
    // These two are what "the voice changed at the cut" means numerically:
    // semitones of pitch and decibels of level, from each utterance to the next.
    double f0_step = 0.0, f0_step_max = 0.0;
    double db_step = 0.0, db_step_max = 0.0;
    int steps = 0;
    for (std::size_t k = 1; k < rec.utterances.size(); ++k) {
        const yoai::voice::Utterance& a = rec.utterances[k - 1];
        const yoai::voice::Utterance& b = rec.utterances[k];
        if (a.f0_mean > 0.0f && b.f0_mean > 0.0f) {
            double st = 12.0 * std::log(b.f0_mean / a.f0_mean) / std::log(2.0);
            if (st < 0.0) st = -st;
            f0_step += st;
            if (st > f0_step_max) f0_step_max = st;
        }
        if (a.rms_mean > 0.0f && b.rms_mean > 0.0f) {
            double db = 20.0 * std::log10(b.rms_mean / a.rms_mean);
            if (db < 0.0) db = -db;
            db_step += db;
            if (db > db_step_max) db_step_max = db;
        }
        ++steps;
    }
    if (steps > 0) {
        f0_step /= steps;
        db_step /= steps;
    }

    std::printf("  %-6s  %5.2f s  %d utterance(s)  F0 %6.1f Hz (sd %4.1f)  "
                "RMS %.4f (sd %.4f)\n",
                label, rec.duration, static_cast<int>(rec.utterances.size()),
                rec.baseline.f0_mean, rec.baseline.f0_sd,
                rec.baseline.rms_mean, rec.baseline.rms_sd);
    std::printf("          step to the next: F0 %.2f st mean / %.2f max, "
                "level %.2f dB mean / %.2f max\n",
                f0_step, f0_step_max, db_step, db_step_max);
    for (std::size_t k = 0; k < rec.utterances.size(); ++k) {
        const yoai::voice::Utterance& u = rec.utterances[k];
        std::printf("            [%d] at %5.2f s  %4.2f s  F0 %6.1f  range %4.1f st  "
                    "RMS %.4f\n",
                    static_cast<int>(k), u.start, u.duration, u.f0_mean, u.f0_range_st,
                    u.rms_mean);
    }
}

// Whole against split, which is the question this class was built to answer.
//
// Not a verdict: nothing here can tell you whether the seams are audible, and
// the two .wav files are the actual deliverable. What the numbers do is say
// where to listen -- a bigger spread of F0 BETWEEN utterances in the split take
// is the voice stepping at the cuts, and a different total duration is the
// pacing changing rather than the timbre.
int compare(const char* talker, const char* tokenizer, int voices, const char* instruct,
            long long seed, float sub_temp, const char* ref_wav, int min_chars) {
    if (!Reader::available()) {
        std::printf("\nthis build has no speech engine -- skipping the live half\n");
        return 0;
    }

    const std::string passage =
        "榨菜是一种常见的下饭菜。它主要是用青菜头腌制而成的，口感比较脆，"
        "味道咸淡适中。你可以把它切碎后拌饭，也可以用来炒菜或者做汤。"
        "很多人早餐配粥的时候都会来一点。你平时喜欢吃吗？";

    Reader reader;
    reader.set_voice(instruct ? instruct : "warm and clear, natural pace, close to the microphone");
    std::printf("\nloading %d voice(s)...\n", voices);
    if (!reader.load(talker, tokenizer, voices)) {
        std::printf("  FAIL load: %s\n", reader.last_error().c_str());
        return 1;
    }
    const std::vector<std::string> named = reader.speakers();
    if (!named.empty()) {
        reader.set_speaker(named[0]);
    }
    // Clone mode A, for the Base checkpoint: one voice for every piece, from a
    // recording reduced to an embedding, or from that embedding saved as a
    // .spk. After load(), which is what the extraction needs.
    //
    // A recording is also written back out as a .spk and read in again, so
    // the file path is checked against the vector it came from: it must load
    // the same values, bit for bit, or the voice it anchors is not the same.
    const std::string ref = ref_wav ? ref_wav : "";
    const bool ref_is_spk = ref.size() > 4 && ref.compare(ref.size() - 4, 4, ".spk") == 0;
    if (ref_is_spk) {
        if (!reader.load_reference_embedding(ref)) {
            std::printf("  FAIL reference: %s\n", reader.last_error().c_str());
            return 1;
        }
        std::printf("  reference embedding from %s, %d values\n", ref.c_str(),
                    static_cast<int>(reader.reference_embedding().size()));
    } else if (!ref.empty()) {
        if (!reader.set_reference_voice(ref)) {
            std::printf("  FAIL reference: %s\n", reader.last_error().c_str());
            return 1;
        }
        const std::vector<float> extracted = reader.reference_embedding();
        std::printf("  reference voice from %s, %d values\n", ref.c_str(),
                    static_cast<int>(extracted.size()));

        const std::string spk = "reference-" + reader.model_type() + ".spk";
        if (!reader.save_reference_embedding(spk) || !reader.load_reference_embedding(spk)) {
            std::printf("  FAIL reference: %s\n", reader.last_error().c_str());
            return 1;
        }
        const std::vector<float>& loaded = reader.reference_embedding();
        const bool same = loaded.size() == extracted.size() &&
                          (extracted.empty() ||
                           std::memcmp(&loaded[0], &extracted[0],
                                       extracted.size() * sizeof(float)) == 0);
        if (!same) {
            std::printf("  FAIL reference: %s does not load back what was saved\n", spk.c_str());
            return 1;
        }
        std::printf("  wrote %s and read it back unchanged\n", spk.c_str());
    }
    reader.set_seed(seed);
    // The one field this sweep is about. Negative leaves the engine's 0.9.
    yoai::voice::QttsSampling sampling;
    if (sub_temp >= 0.0f) {
        sampling.subtalker_temperature = sub_temp;
        reader.set_sampling(sampling);
    }
    std::printf("  %s, %d voice(s), speaker %s, seed %lld\n", reader.model_type().c_str(),
                reader.voices(), named.empty() ? "(none)" : named[0].c_str(), seed);
    std::printf("  sub-talker temperature %.2f\n",
                sub_temp >= 0.0f ? sub_temp : sampling.subtalker_temperature);

    int failures = 0;

    // A maximum that cannot be reached and a minimum that is never satisfied:
    // no cut is legal anywhere, so the passage stays whole and goes through
    // exactly the same code as the split take.
    SplitOptions unsplit;
    unsplit.min_chars = 1000000;
    unsplit.max_chars = 1000000;

    // How coarse the cut is, which is the one knob left on the pace. A bigger
    // minimum puts several sentences in each piece, so the model plans a pace
    // across them again -- and it is the same number the streaming case calls
    // "how many characters before sending the first piece to the engine", so
    // the sweep and the product decision are the same sweep.
    SplitOptions split_opt;
    if (min_chars > 0) {
        split_opt.min_chars = static_cast<std::size_t>(min_chars);
        if (split_opt.max_chars < split_opt.min_chars * 2) {
            split_opt.max_chars = split_opt.min_chars * 2;
        }
    }
    const std::vector<Sentence> split_cut = split_sentences(passage, split_opt);

    std::vector<float> whole_pcm, split_pcm;
    std::vector<std::size_t> split_starts;
    ReaderResult whole_r, split_r;

    // Whole FIRST, so if anything drifts with heat it drifts against the split
    // take rather than in its favour.
    std::printf("\n  -- whole, one utterance --\n");
    if (!one_pass(reader, passage, unsplit, "whole", &whole_pcm, 0, &whole_r, &failures)) {
        return failures;
    }
    std::printf("\n  -- split, min_chars %d -> %d piece(s) --\n",
                static_cast<int>(split_opt.min_chars), static_cast<int>(split_cut.size()));
    if (!one_pass(reader, passage, split_opt, "split", &split_pcm, &split_starts,
                  &split_r, &failures)) {
        return failures;
    }

    std::printf("\n  cost\n");
    std::printf("    whole  %d piece(s)  %5.2f s of speech in %5.2f s (%.2fx)  "
                "first audio %.2f s\n",
                whole_r.sentences, whole_r.speech_seconds, whole_r.elapsed,
                whole_r.speech_seconds > 0.0f ? whole_r.elapsed / whole_r.speech_seconds : 0.0,
                whole_r.first_audio_seconds);
    std::printf("    split  %d piece(s)  %5.2f s of speech in %5.2f s (%.2fx)  "
                "first audio %.2f s\n",
                split_r.sentences, split_r.speech_seconds, split_r.elapsed,
                split_r.speech_seconds > 0.0f ? split_r.elapsed / split_r.speech_seconds : 0.0,
                split_r.first_audio_seconds);

    // And the same passage again through begin/feed/finish, two characters at a
    // time, which is about what one token of Chinese is.
    std::printf("\n  -- streamed, 2 characters at a time --\n");
    std::vector<float> stream_pcm;
    ReaderResult stream_r;
    streamed_pass(reader, passage, split_opt, 2, &stream_pcm, &stream_r, &failures);
    if (stream_r.sentences != split_r.sentences) {
        std::printf("  FAIL stream: %d piece(s) streamed against %d handed over whole\n",
                    stream_r.sentences, split_r.sentences);
        ++failures;
    }
    std::printf("    stream %d piece(s)  %5.2f s of speech in %5.2f s (%.2fx)  "
                "first audio %.2f s\n",
                stream_r.sentences, stream_r.speech_seconds, stream_r.elapsed,
                stream_r.speech_seconds > 0.0f ? stream_r.elapsed / stream_r.speech_seconds : 0.0,
                stream_r.first_audio_seconds);

    // Later pieces allowed to wait, against a writer as fast as the speech and
    // one far faster. The reader's own log lines show where each piece fell.
    {
        SplitOptions paced = split_opt;
        paced.min_chars       = 12;
        paced.clause_cuts     = true;
        paced.later_min_chars = 30;
        paced.later_max_chars = 80;
        const int speeds[] = {200, 20};   // 5 and 50 characters a second
        for (int s = 0; s < 2; ++s) {
            std::printf("\n  -- paced, later pieces from 30, %d ms a character --\n", speeds[s]);
            ReaderResult r;
            if (paced_pass(reader, passage, paced, speeds[s], &r, &failures)) {
                std::printf("    paced  %d piece(s)  %5.2f s of speech in %5.2f s  "
                            "first audio %.2f s  starved %.2f s\n",
                            r.sentences, r.speech_seconds, r.elapsed,
                            r.first_audio_seconds, r.starved_seconds);
            }
        }
    }

    std::printf("\n  voice\n");
    describe("whole", whole_pcm);
    describe("split", split_pcm);
    describe_pieces(split_pcm, split_starts, split_cut);

    // Named per seed, so several runs can be lined up rather than each one
    // overwriting the last -- which is how the first set of these was lost.
    // The checkpoint is in the name because it has to be: a base run reading a
    // custom_voice take as its reference wrote over that take, so the next run
    // cloned the clone. Two configurations that produce comparable audio need
    // names that cannot collide.
    char wa[96], sa[96];
    const std::string kind = reader.model_type();
    std::snprintf(wa, sizeof(wa), "whole-%s-seed%lld-t%.2f.wav", kind.c_str(), seed,
                  sub_temp >= 0.0f ? sub_temp : 0.9f);
    std::snprintf(sa, sizeof(sa), "split-%s-seed%lld-m%d.wav", kind.c_str(), seed,
                  static_cast<int>(split_opt.min_chars));
    write_wav(wa, whole_pcm, Reader::sample_rate());
    write_wav(sa, split_pcm, Reader::sample_rate());
    std::printf("\n  wrote %s and %s -- the comparison is by ear\n", wa, sa);
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    SplitOptions tight;
    tight.min_chars = 4;
    tight.max_chars = 20;
    // The tail rule has its own case; here it would merge these short passages
    // back into one piece and hide what the cut cases are checking.
    tight.tail_chars = 1;

    // 1. Ordinary Chinese sentences, each long enough to stand alone.
    {
        const std::string text =
            "榨菜是一种常见的下饭菜。它的口感比较脆，味道咸淡适中。你平时喜欢吃吗？";
        const std::vector<Sentence> out = split_checked(text, tight, "chinese");
        show("chinese", out);
        expect(out.size() == 3, "chinese", "expected three pieces");
    }

    // 2. The case that lost text. Two sentences below min_chars: neither may
    //    disappear, and neither may become an utterance of its own.
    {
        const std::string text = "你好。你觉得呢？我们明天再聊这件事情吧。";
        const std::vector<Sentence> out = split_checked(text, tight, "short");
        show("short", out);
        for (std::size_t k = 0; k < out.size(); ++k) {
            expect(chars_of(out[k].text) >= tight.min_chars || out.size() == 1,
                   "short", "a piece is below min_chars");
        }
    }

    // 3. A tail too short to stand alone joins the piece before it rather than
    //    becoming one, because nothing follows it to absorb it.
    {
        const std::string text = "这是第一句话，说得比较长一些。好的。";
        const std::vector<Sentence> out = split_checked(text, tight, "tail");
        show("tail", out);
        expect(out.size() == 1, "tail", "the short tail should have joined the piece before it");
    }

    // 4. Closing punctuation belongs to the sentence that ended, not to the
    //    next one, or it is read aloud as a stray character.
    {
        const std::string text = "他说：「我明天就去。」然后他就走了，没有再回头。";
        const std::vector<Sentence> out = split_checked(text, tight, "closers");
        show("closers", out);
        if (out.size() >= 2) {
            expect(out[1].text.compare(0, 3, "」") != 0, "closers",
                   "the next piece starts with a closing bracket");
        }
    }

    // 5. A decimal point is not a full stop. "3.14" must survive whole.
    {
        const std::string text = "圆周率约等于 3.14 这个数字，你应该已经很熟悉了。";
        const std::vector<Sentence> out = split_checked(text, tight, "decimal");
        show("decimal", out);
        bool intact = false;
        for (std::size_t k = 0; k < out.size(); ++k) {
            if (out[k].text.find("3.14") != std::string::npos) {
                intact = true;
            }
        }
        expect(intact, "decimal", "3.14 was cut in half");
    }

    // 6. A run-on with no full stop is cut at a comma rather than growing
    //    without limit.
    {
        const std::string text =
            "第一件事情是这样的，第二件事情是那样的，第三件事情又不太一样，"
            "第四件事情还要再说一次，第五件事情就到此为止";
        const std::vector<Sentence> out = split_checked(text, tight, "runon");
        show("runon", out);
        expect(out.size() > 1, "runon", "a run-on was not cut at all");
        for (std::size_t k = 0; k + 1 < out.size(); ++k) {
            expect(chars_of(out[k].text) <= tight.max_chars, "runon",
                   "a piece ran past max_chars");
        }
    }

    // 7. Text with no punctuation at all still has to be cut somewhere.
    {
        SplitOptions narrow;
        narrow.min_chars = 2;
        narrow.max_chars = 5;
        narrow.tail_chars = 1;
        const std::string text = "一二三四五六七八九十十一十二";
        const std::vector<Sentence> out = split_checked(text, narrow, "nopunct");
        show("nopunct", out);
        expect(out.size() > 1, "nopunct", "unpunctuated text was not cut");
    }

    // 8. Degenerate inputs, which are where a cursor bug turns into a hang.
    {
        split_checked("", tight, "empty");
        split_checked("。", tight, "terminator only");
        split_checked("   \n  ", tight, "whitespace only");
        split_checked("a", tight, "one byte");
        // A truncated UTF-8 sequence: the last character is one byte short.
        std::string truncated = "你好";
        truncated.erase(truncated.size() - 1);
        split_checked(truncated, tight, "truncated utf-8");
        std::printf("degenerate inputs -> no hang, ranges intact\n");
    }

    // 9. The last piece gets its own, higher minimum, because a short final
    //    utterance is the one the model ends without a release -- measured, and
    //    the table is in SplitOptions::tail_chars.
    {
        SplitOptions opt;
        opt.min_chars  = 4;
        opt.max_chars  = 20;
        opt.tail_chars = 16;
        const std::string text = "第一句话在这里。第二句话也在这里。好的。";
        const std::vector<Sentence> out = split_checked(text, opt, "tail rule");
        show("tail rule", out);
        if (!out.empty()) {
            expect(chars_of(out.back().text) >= opt.tail_chars, "tail rule",
                   "the last piece is below tail_chars");
        }
        // And it must not do it by losing anything, which split_checked already
        // verified -- the point of merging BACKWARDS rather than dropping.
        expect(out.size() < 3, "tail rule", "the short tail did not merge");
    }

    // 10. Clause cuts: a comma ends a piece once it is long enough.
    //
    //     The case is a real reply, and the reason this option exists. Its one
    //     full stop is at the very end, so without clause cuts nothing can be
    //     spoken until the model has written all 45 characters -- which is what
    //     a listener reported as "it still waits for the whole thing".
    {
        const std::string text =
            "瘦西湖位于中国江苏省扬州市，是扬州城西一条狭长的人工湖，"
            "因湖面曲折如一条瘦长的青螺而得名。";

        SplitOptions off;
        off.min_chars  = 12;
        off.max_chars  = 40;
        off.tail_chars = 1;
        const std::vector<Sentence> a = split_checked(text, off, "clause off");
        show("clause off", a);

        SplitOptions on = off;
        on.clause_cuts = true;
        const std::vector<Sentence> b = split_checked(text, on, "clause on");
        show("clause on", b);

        expect(b.size() > a.size(), "clause cuts",
               "a comma did not end a piece");
        // The first piece is what decides when the voice starts, so it is the
        // one worth naming: everything up to the first comma past min_chars.
        if (!b.empty()) {
            expect(chars_of(b[0].text) < chars_of(a[0].text), "clause cuts",
                   "the first piece did not get shorter");
        }
    }

    // 11. English, since a reply mixes it in.
    {
        const std::string text =
            "This is the first sentence. Here is a second one! And a third?";
        SplitOptions latin;
        latin.min_chars = 8;
        latin.max_chars = 40;
        latin.tail_chars = 1;
        const std::vector<Sentence> out = split_checked(text, latin, "english");
        show("english", out);
        expect(out.size() == 3, "english", "expected three pieces");
    }

    // 12. The stream, one character at a time, as a language model writes.
    //
    //     Three things: nothing is lost across feed() and finish(); a fragment
    //     still mid-sentence is never handed over; and the first piece comes
    //     out when its clause is complete, not when the whole reply is.
    {
        const std::string text =
            "瘦西湖位于中国江苏省扬州市，是扬州城西一条狭长的人工湖，"
            "因湖面曲折如一条瘦长的青螺而得名。";
        SplitOptions opt;
        opt.min_chars   = 12;
        opt.max_chars   = 40;
        opt.tail_chars  = 1;
        opt.clause_cuts = true;

        SentenceStream stream(opt);
        std::vector<std::string> got;
        std::size_t first_at = 0;   // characters written when piece 0 came out
        std::size_t written  = 0;
        for (std::size_t i = 0; i < text.size(); ) {
            // This passage is CJK and ASCII only, so a character is 3 bytes or 1.
            const std::size_t len = (static_cast<unsigned char>(text[i]) & 0x80) ? 3 : 1;
            const std::vector<std::string> ready = stream.feed(text.substr(i, len));
            i += len;
            ++written;
            if (!ready.empty() && got.empty()) {
                first_at = written;
            }
            got.insert(got.end(), ready.begin(), ready.end());
        }
        const std::vector<std::string> rest = stream.finish();
        got.insert(got.end(), rest.begin(), rest.end());

        std::printf("stream -> %d piece(s), first after %d of %d chars\n",
                    static_cast<int>(got.size()), static_cast<int>(first_at),
                    static_cast<int>(chars_of(text)));
        std::string joined;
        for (std::size_t k = 0; k < got.size(); ++k) {
            std::printf("    [%d] %s\n", static_cast<int>(k), got[k].c_str());
            joined += got[k];
        }
        expect(joined == text, "stream", "the streamed pieces do not reproduce the passage");
        expect(first_at > 0 && first_at < chars_of(text), "stream",
               "the first piece waited for the whole reply");

        // A trailing fragment is not settled, however long it is.
        SentenceStream open(opt);
        expect(open.feed("这是一个还没有写完而且足够长的句子").empty(), "stream",
               "a fragment still mid-sentence was handed over");
    }

    // 13. Fed whole, the stream gives the same pieces the splitter does, trimmed.
    //     read() is exactly this path, so it is what a passage handed over in one
    //     go is cut into.
    {
        const std::string text =
            "This is the first sentence. Here is a second one! And a third?";
        SplitOptions latin;
        latin.min_chars  = 8;
        latin.max_chars  = 40;
        latin.tail_chars = 1;

        SentenceStream stream(latin);
        std::vector<std::string> got = stream.feed(text);
        const std::vector<std::string> rest = stream.finish();
        got.insert(got.end(), rest.begin(), rest.end());

        const std::vector<Sentence> ref = split_sentences(text, latin);
        bool same = got.size() == ref.size();
        for (std::size_t k = 0; same && k < got.size(); ++k) {
            const std::string& r = ref[k].text;
            const std::size_t e = r.find_last_not_of(' ');
            same = got[k] == r.substr(0, e + 1);
        }
        std::printf("stream whole -> %d piece(s), %s the splitter\n",
                    static_cast<int>(got.size()), same ? "same as" : "DIFFERENT from");
        expect(same, "stream whole", "fed in one go, the stream cut differently");
    }

    // 14. What reaches the cutter at all: markdown turned into speech.
    check_speakable();

    // 15. A closing quote ends a sentence only after a terminator. Mid-sentence
    //     it closes a quotation, and the stream must not hand the piece over
    //     there -- it did, and "的地方（断桥）。" was spoken as a piece alone.
    {
        SplitOptions opt;
        opt.min_chars   = 12;
        opt.max_chars   = 40;
        opt.tail_chars  = 1;
        opt.clause_cuts = true;

        const std::string text = "也是《白蛇传》中“白娘子和许仙相遇”的地方（断桥）。";
        SentenceStream stream(opt);
        std::vector<std::string> got;
        for (std::size_t i = 0; i < text.size(); ) {
            const std::size_t len = (static_cast<unsigned char>(text[i]) & 0x80) ? 3 : 1;
            const std::vector<std::string> ready = stream.feed(text.substr(i, len));
            got.insert(got.end(), ready.begin(), ready.end());
            i += len;
        }
        const std::vector<std::string> rest = stream.finish();
        got.insert(got.end(), rest.begin(), rest.end());
        std::printf("stream quote -> %d piece(s)\n", static_cast<int>(got.size()));
        for (std::size_t k = 0; k < got.size(); ++k) {
            std::printf("    [%d] %s\n", static_cast<int>(k), got[k].c_str());
        }
        expect(got.size() == 1, "stream quote", "cut at a closing quote mid-sentence");

        // And after a terminator it still settles at once, without waiting for
        // the next sentence to begin.
        SentenceStream quoted(opt);
        const std::vector<std::string> now = quoted.feed("他说：“我明天一早就去那里看看。”");
        expect(now.size() == 1, "stream quote",
               "a quotation ending in a full stop was not settled");
    }

    // 16. Later pieces wait for more text; an urgent one does not.
    //
    //     The first piece is cut short, because the listener is waiting for
    //     it. The ones after it end only at a full stop from later_min_chars
    //     on. And when the reader says the audio is about to run out, the next
    //     piece is cut by the first piece's rules again -- once.
    {
        SplitOptions opt;
        opt.min_chars       = 12;
        opt.max_chars       = 40;
        opt.tail_chars      = 1;
        opt.clause_cuts     = true;
        opt.later_min_chars = 30;
        opt.later_max_chars = 80;

        const std::string first  = "太湖位于中国江苏省南部和浙江省北部。";
        const std::string second = "它是我国第三大淡水湖，也是长江下游南岸最大的湖泊。";
        const std::string third  = "湖中有大小岛屿五十多个，其中西山岛最大。";
        const std::string fourth = "沿岸有无锡、苏州、湖州等城市，风景秀丽，物产丰富。";
        const std::string text = first + second + third + fourth;

        // Fed one character at a time, as a writer would, and fed whole, as
        // a fast one effectively does: the same cuts either way.
        for (int whole = 0; whole < 2; ++whole) {
            SentenceStream stream(opt);
            std::vector<std::string> got;
            if (whole) {
                got = stream.feed(text);
            } else {
                for (std::size_t i = 0; i < text.size(); i += 3) {   // CJK only
                    const std::vector<std::string> ready = stream.feed(text.substr(i, 3));
                    got.insert(got.end(), ready.begin(), ready.end());
                }
            }
            const std::vector<std::string> rest = stream.finish();
            got.insert(got.end(), rest.begin(), rest.end());

            std::printf("later pieces, %s -> %d piece(s)\n", whole ? "fed whole" : "streamed",
                        static_cast<int>(got.size()));
            for (std::size_t k = 0; k < got.size(); ++k) {
                std::printf("    [%d] %2d chars  %s\n", static_cast<int>(k),
                            static_cast<int>(chars_of(got[k])), got[k].c_str());
            }
            std::string joined;
            for (std::size_t k = 0; k < got.size(); ++k) {
                joined += got[k];
            }
            expect(joined == text, "later pieces", "the pieces do not reproduce the passage");
            expect(!got.empty() && got[0] == first, "later pieces",
                   "the first piece is not the first sentence");
            // At least the second and third sentences: 24 characters is below
            // later_min_chars, so the second piece runs on to the next full
            // stop. Fed whole it takes the fourth as well, because a remainder
            // shorter than the minimum joins the piece before it.
            const std::string both = second + third;
            expect(got.size() >= 2 && got[1].compare(0, both.size(), both) == 0,
                   "later pieces",
                   "the second piece did not wait for a full stop past later_min_chars");
        }

        // Urgent: the reader is running dry after the first piece, so the next
        // is cut as soon as the first piece's rules allow, and only that one.
        {
            SentenceStream stream(opt);
            std::vector<std::string> got = stream.feed(first);
            stream.set_urgent(true);
            for (std::size_t i = 0; i < second.size() + third.size(); i += 3) {
                const std::vector<std::string> ready =
                    stream.feed((second + third).substr(i, 3));
                got.insert(got.end(), ready.begin(), ready.end());
            }
            expect(got.size() == 2 && got[1] == second, "urgent",
                   "an urgent piece was not cut by the first piece's rules");
            expect(!stream.urgent(), "urgent", "urgency outlived the piece it was for");
        }

        // And the case the pacer exists for: a finished sentence already in the
        // stream, too short to go by the later rules, with no more text coming
        // for now. poll() after set_urgent() hands it over.
        {
            SentenceStream stream(opt);
            stream.feed(first);
            expect(stream.feed(second).empty(), "poll",
                   "a later piece below later_min_chars was handed over without waiting");
            stream.set_urgent(true);
            const std::vector<std::string> now = stream.poll();
            expect(now.size() == 1 && now[0] == second, "poll",
                   "an urgent poll did not hand over the finished sentence");
        }
    }

    // 17. The first piece ends at a full stop from stop_chars on, so a short
    //     opening sentence is spoken at once -- but not one short enough to be
    //     read clipped, and not at a comma, and not for the pieces after it.
    {
        SplitOptions opt;
        opt.min_chars       = 12;
        opt.max_chars       = 40;
        opt.tail_chars      = 1;
        opt.clause_cuts     = true;
        opt.later_min_chars = 30;
        opt.later_max_chars = 80;
        opt.stop_chars      = 6;

        struct Case {
            const char* text;
            const char* first;   // what the first piece must be
        };
        const Case cases[] = {
            // 7 characters and a full stop: alone.
            {"我知道苏州河。它是上海的一条重要河流，流经中心城区，最后汇入黄浦江。",
             "我知道苏州河。"},
            // 3 characters: still joined to what follows.
            {"你好。我是 YoAI 的助手，很高兴认识你。", "你好。我是 YoAI 的助手，"},
            // A comma at 7 is not a full stop: min_chars still applies.
            {"苏州河在上海市区，它流经中心城区，最后汇入黄浦江。", "苏州河在上海市区，它流经中心城区，"},
        };
        for (std::size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
            const std::string text = cases[c].text;
            SentenceStream stream(opt);
            std::vector<std::string> got;
            for (std::size_t i = 0; i < text.size(); ) {
                const std::size_t len = (static_cast<unsigned char>(text[i]) & 0x80) ? 3 : 1;
                const std::vector<std::string> ready = stream.feed(text.substr(i, len));
                got.insert(got.end(), ready.begin(), ready.end());
                i += len;
            }
            const std::vector<std::string> rest = stream.finish();
            got.insert(got.end(), rest.begin(), rest.end());
            std::printf("first stop -> %s\n", got.empty() ? "(nothing)" : got[0].c_str());
            expect(!got.empty() && got[0] == cases[c].first, "first stop",
                   std::string("expected the first piece to be ") + cases[c].first);
        }

        // Only the first: a short sentence later waits for later_min_chars.
        SentenceStream stream(opt);
        stream.feed("我知道苏州河。");
        expect(stream.feed("它很长。").empty(), "first stop",
               "a short later sentence was cut as if it were the first");
    }

    if (argc >= 3) {
        const int voices = (argc >= 4) ? std::atoi(argv[3]) : 1;
        const long long seed = (argc >= 6) ? std::atoll(argv[5]) : 23;
        const float sub_temp = (argc >= 7) ? static_cast<float>(std::atof(argv[6])) : -1.0f;
        g_failures += compare(argv[1], argv[2], voices, (argc >= 5) ? argv[4] : 0, seed, sub_temp,
                              (argc >= 8) ? argv[7] : 0,
                              (argc >= 9) ? std::atoi(argv[8]) : 0);
    }

    if (g_failures == 0) {
        std::printf("\nreader: every case passed\n");
        return 0;
    }
    std::printf("\nreader: %d check(s) failed\n", g_failures);
    return 1;
}
