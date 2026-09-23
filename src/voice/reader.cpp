#include "voice/reader.hpp"

#include "platform/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace bodyclock {
namespace voice {

namespace {

typedef std::chrono::steady_clock clk;

double seconds_since(clk::time_point start) {
    return std::chrono::duration_cast<std::chrono::duration<double> >(clk::now() - start).count();
}

//------------------------------------------------------------------------------
// UTF-8
//------------------------------------------------------------------------------

// Bytes in the sequence starting with `lead`. **1 for anything malformed**, so
// a stray continuation byte advances the cursor instead of leaving it where it
// was -- the difference between a garbled piece and a hang.
std::size_t utf8_len(unsigned char lead) {
    if (lead < 0x80)          return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

// The codepoint at `i`, with its length. A truncated or malformed sequence
// decodes as the lead byte itself, which no rule below matches, so it simply
// becomes an ordinary character.
unsigned decode(const std::string& s, std::size_t i, std::size_t* len) {
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    std::size_t n = utf8_len(lead);
    if (i + n > s.size()) {
        n = 1;
    }
    *len = n;
    if (n == 1) {
        return lead;
    }
    static const unsigned kLeadMask[5] = {0, 0, 0x1F, 0x0F, 0x07};
    unsigned cp = lead & kLeadMask[n];
    for (std::size_t k = 1; k < n; ++k) {
        const unsigned char c = static_cast<unsigned char>(s[i + k]);
        if ((c & 0xC0) != 0x80) {   // not a continuation byte: malformed
            *len = 1;
            return lead;
        }
        cp = (cp << 6) | (c & 0x3F);
    }
    return cp;
}

bool is_space(unsigned cp) {
    return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == 0x3000;
}

bool is_digit(unsigned cp) {
    return cp >= '0' && cp <= '9';
}

// Ends a sentence outright. The full-width forms are the ones that matter for
// this app's text; the ASCII ones are here because a reply mixes them in.
//
// '.' is NOT in this set -- see terminates_here().
bool is_hard_stop(unsigned cp) {
    return cp == 0x3002     // 。
        || cp == 0xFF01     // ！
        || cp == 0xFF1F     // ？
        || cp == 0xFF1B     // ；
        || cp == 0x2026     // …
        || cp == '!' || cp == '?' || cp == ';';
}

// Punctuation that belongs to the sentence that just ended, so the cut goes
// AFTER it: a closing quote or bracket left at the head of the next piece is
// read aloud as a stray character.
bool is_closer(unsigned cp) {
    return cp == 0xFF09     // ）
        || cp == 0x300D     // 」
        || cp == 0x300F     // 』
        || cp == 0x201D     // ”
        || cp == 0x2019     // ’
        || cp == 0x3011     // 】
        || cp == 0x300B     // 》
        || cp == ')' || cp == ']' || cp == '"' || cp == '\'';
}

// Comma-level. The fallback when a piece has run past max_chars with no full
// stop in it -- and, with SplitOptions::clause_cuts on, a cut in its own right
// once the piece is long enough.
bool is_secondary(unsigned cp) {
    return cp == 0xFF0C     // ，
        || cp == 0x3001     // 、
        || cp == 0xFF1A     // ：
        || cp == ',' || cp == ':';
}

// Whether the character at `pos` (which decoded to `cp`, `len` bytes) ends a
// sentence here.
//
// The whole of the subtlety is the ASCII period. "3.14" and "v1.2" are one
// token each and cutting inside them produces two pieces the engine reads as
// "three point" and "fourteen". So a '.' ends a sentence only when it is not
// between two digits -- which keeps "1.5 倍。" working and costs nothing that
// this app's text actually contains. Abbreviations ("Dr.", "e.g.") are not
// handled and would cut early; they do not appear in Chinese replies, and the
// failure is a seam rather than a wrong reading.
bool terminates_here(const std::string& s, std::size_t pos, unsigned cp, std::size_t len,
                     unsigned prev_cp) {
    if (is_hard_stop(cp)) {
        return true;
    }
    if (cp != '.') {
        return false;
    }
    if (is_digit(prev_cp)) {
        const std::size_t next = pos + len;
        if (next < s.size() && is_digit(static_cast<unsigned char>(s[next]))) {
            return false;
        }
    }
    return true;
}

// Codepoints, which is the unit SplitOptions is expressed in.
std::size_t chars_in(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ) {
        std::size_t len;
        decode(s, i, &len);
        i += len;
        ++n;
    }
    return n;
}

// ASCII whitespace plus the ideographic space, from both ends. What reaches the
// engine, never what the Sentence records.
std::string trimmed(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size()) {
        std::size_t len;
        const unsigned cp = decode(s, b, &len);
        if (!is_space(cp)) {
            break;
        }
        b += len;
    }
    std::size_t e = s.size();
    while (e > b) {
        // Walk back to the start of the last character, then test it.
        std::size_t start = e - 1;
        while (start > b && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) {
            --start;
        }
        std::size_t len;
        const unsigned cp = decode(s, start, &len);
        if (!is_space(cp)) {
            break;
        }
        e = start;
    }
    return s.substr(b, e - b);
}

//------------------------------------------------------------------------------
// The edges of a piece
//------------------------------------------------------------------------------
//
// Three defects in the concatenation, all of them ours rather than the model's,
// all measured on one passage and all fixed here.
//
//   1. **The pause at a join was twice as long as a natural one.** Each piece
//      carries its own silence at both ends and they add: 0.30 s of trailing
//      plus 0.35 s of leading came to 0.65 s, against 0.28-0.52 s for the
//      pauses the model puts between sentences when it reads the passage whole.
//
//   2. **The noise floor had a hole in it.** A piece's last 20 ms decayed to
//      digital zero while the room tone either side of the join sat at -62 to
//      -68 dBFS, so the hiss stopped and restarted -- audible, and nothing to
//      do with the speech.
//
//   3. **A piece can end while it is still sounding.** Four takes out of four
//      cut at a fine split ended with the final vowel still at -33 to -45 dBFS
//      and then gated off inside 10 ms. That part IS the model -- it stops
//      generating, and no amount of editing puts the release back -- but the
//      10 ms gate is a click, and a fade is not.
//
// The shaper streams: it drops leading silence as it arrives, holds a trailing
// run of quiet in case more speech follows it, and keeps 20 ms of speech back so
// there is something to fade if the piece turns out to end hot. Nothing is
// delayed while the voice is actually speaking.
//
// **It never synthesizes a pause.** The first attempt did, filling a join with
// zeros when the piece before it had no trailing room tone to spare, and that
// made defect 2 worse rather than better -- a 20 ms hole became a 300 ms one.
// A pause is assembled out of whatever real room tone the two pieces have
// between them: as much of the previous piece's tail as it can spare, and the
// rest taken from the next piece's lead instead of being thrown away.

// Analysis frame, and the level below which a frame is not speech.
//
// -55 dBFS sits in a gap that was measured rather than guessed: the room tone
// in these recordings runs -62 to -68 dBFS, and the quietest speech at the end
// of an utterance reaches about -45 before the engine stops. Well clear of
// both, and only frames between them are at risk of being misjudged.
const double kFrameSeconds = 0.010;
const double kFloorDbfs    = -55.0;

// How much of a piece's own trailing room tone to keep at a join, which is
// therefore also the pause. 0.30 s is the median of the pauses the model chose
// for itself when reading this passage as one utterance.
const double kJoinPause = 0.30;

// Speech held back, so a piece that ends hot has something to fade.
const double kEdgeSeconds = 0.020;

bool above_floor(const float* f, std::size_t n) {
    if (n == 0) {
        return false;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sum += static_cast<double>(f[i]) * f[i];
    }
    const double rms = std::sqrt(sum / static_cast<double>(n));
    return rms > std::pow(10.0, kFloorDbfs / 20.0);
}

// Linear to zero over the whole buffer. Short enough that it is heard as the
// sound stopping rather than as a fade.
void fade_out(std::vector<float>* v) {
    const std::size_t n = v->size();
    for (std::size_t i = 0; i < n; ++i) {
        (*v)[i] = static_cast<float>((*v)[i] *
                                     (1.0 - static_cast<double>(i + 1) / static_cast<double>(n)));
    }
}

// One piece's worth of edge state. Reset between pieces.
class EdgeShaper {
public:
    explicit EdgeShaper(int rate)
        : frame_(static_cast<std::size_t>(kFrameSeconds * rate)),
          edge_(static_cast<std::size_t>(kEdgeSeconds * rate)),
          join_(static_cast<std::size_t>(kJoinPause * rate)),
          head_done_(false),
          lead_budget_(0) {
        if (frame_ == 0) {
            frame_ = 1;
        }
    }

    // Take one handover and append what should go out to `out`.
    void feed(const std::vector<float>& in, std::vector<float>* out) {
        carry_.insert(carry_.end(), in.begin(), in.end());
        std::size_t i = 0;
        for (; i + frame_ <= carry_.size(); i += frame_) {
            const float* f = &carry_[i];
            if (above_floor(f, frame_)) {
                head_done_ = true;
                // Whatever was being held was an internal pause after all, not
                // the end of the piece, so it goes out in order and untouched.
                flush(&keep_, out);
                flush(&quiet_, out);
                keep_.insert(keep_.end(), f, f + frame_);
                while (keep_.size() > edge_) {
                    out->push_back(keep_.front());
                    keep_.erase(keep_.begin());
                }
            } else if (head_done_) {
                quiet_.insert(quiet_.end(), f, f + frame_);
            } else if (lead_budget_ > 0) {
                // Leading silence, kept only as far as the join still owes a
                // pause. This is REAL room tone, which is why the pause is made
                // of it rather than of zeros -- see the note above the class.
                const std::size_t n = frame_ < lead_budget_ ? frame_ : lead_budget_;
                out->insert(out->end(), f, f + n);
                lead_budget_ -= n;
            }
            // else: leading silence past what the pause needs, dropped.
        }
        carry_.erase(carry_.begin(), carry_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    // No more audio for this piece. `last` asks for the end of the passage
    // rather than a join.
    void finish(bool last, std::vector<float>* out) {
        // The partial frame nobody judged. Too short to classify, and dropping
        // it would clip a word.
        if (!carry_.empty() && head_done_) {
            keep_.insert(keep_.end(), carry_.begin(), carry_.end());
        }
        carry_.clear();

        // The engine fades its own tail to digital zero over the last few
        // milliseconds. Keeping that is what left a hole in the room tone, so
        // it comes off before anything else is decided.
        drop_dead_tail(&quiet_);

        if (quiet_.empty()) {
            // Ended while still sounding. Fade what was held, which is the only
            // part of this the editing side can do anything about, and leave the
            // whole pause for the next piece's own room tone to supply.
            fade_out(&keep_);
            flush(&keep_, out);
            lead_budget_ = join_;
        } else {
            flush(&keep_, out);
            if (quiet_.size() > join_) {
                quiet_.resize(join_);
            }
            // Whatever this piece could not supply, the next one's leading room
            // tone does. Nothing is ever synthesized.
            lead_budget_ = join_ - quiet_.size();
            flush(&quiet_, out);
        }
        if (last) {
            lead_budget_ = 0;
            // **Unconditionally**, and that is a correction rather than belt and
            // braces. The fade above is reached only when the piece ends with
            // nothing quiet after it, and the engine always leaves a few
            // milliseconds of its own ramp -- enough to be classified as quiet
            // and to make `quiet_` non-empty -- so on the take that prompted
            // all this the fade never fired. On an ending that was already
            // released this is 20 ms of room tone faded, which is nothing; on
            // one cut mid-vowel it is the difference between a gate and a fade.
            fade_tail(out, edge_);
        }
        head_done_ = false;
    }

private:
    static void flush(std::vector<float>* from, std::vector<float>* out) {
        out->insert(out->end(), from->begin(), from->end());
        from->clear();
    }

    // Ramp the last `n` samples of whatever is about to go out down to zero.
    static void fade_tail(std::vector<float>* out, std::size_t n) {
        const std::size_t total = out->size();
        if (total == 0) {
            return;
        }
        if (n > total) {
            n = total;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const double g = 1.0 - static_cast<double>(i + 1) / static_cast<double>(n);
            (*out)[total - n + i] = static_cast<float>((*out)[total - n + i] * g);
        }
    }

    // Trim a run of quiet back to where it was still room tone. Below about
    // -85 dBFS there is nothing left but the engine's own ramp to zero, and
    // that ramp is the hole: room tone, then nothing, then the next piece.
    static void drop_dead_tail(std::vector<float>* v) {
        const double floor = std::pow(10.0, -85.0 / 20.0);
        std::size_t n = v->size();
        while (n > 0 && std::fabs((*v)[n - 1]) < floor) {
            --n;
        }
        v->resize(n);
    }

    std::size_t frame_;
    std::size_t edge_;
    std::size_t join_;
    bool        head_done_;
    std::size_t lead_budget_;   // leading quiet the next join still owes
    std::vector<float> carry_;   // samples not yet a whole frame
    std::vector<float> keep_;    // speech held back for a possible fade
    std::vector<float> quiet_;   // trailing run of silence, fate undecided
};

}  // namespace

//------------------------------------------------------------------------------
// Splitting
//------------------------------------------------------------------------------

std::vector<Sentence> split_sentences(const std::string& text, const SplitOptions& opt) {
    std::vector<Sentence> out;
    if (text.empty()) {
        return out;
    }

    const std::size_t min_chars = opt.min_chars;
    // A maximum below the minimum would cut every piece before it was allowed
    // to be cut, which is not a configuration anybody means. The minimum wins,
    // because it is the one that exists to prevent a specific failure.
    std::size_t max_chars = opt.max_chars > 0 ? opt.max_chars : 1;
    if (max_chars < min_chars) {
        max_chars = min_chars;
    }

    // Every cut goes through here, so the invariant has one place to hold: a
    // piece starts where the previous one ended, and `begin` is only ever moved
    // to the `end` just recorded.
    std::size_t begin = 0;
    std::size_t chars = 0;
    std::size_t last_secondary = 0;   // byte offset just past the last comma
    unsigned prev_cp = 0;
    std::size_t i = 0;

    while (i < text.size()) {
        std::size_t len;
        const unsigned cp = decode(text, i, &len);
        const std::size_t cur = i;
        i += len;
        ++chars;

        if (is_secondary(cp)) {
            last_secondary = i;
        }

        // A newline is a boundary in its own right once the piece is long
        // enough. A reply with a bulleted list has no full stops in it and
        // would otherwise be cut only by max_chars, in the middle of items.
        const bool newline = (cp == '\n');
        // A comma is a pause, and once a piece is long enough to stand alone
        // there is nothing to gain by running past one. See clause_cuts.
        const bool clause = opt.clause_cuts && is_secondary(cp) && chars >= min_chars;
        if (terminates_here(text, cur, cp, len, prev_cp) || clause ||
            (newline && chars >= min_chars)) {
            // Take the closers and any further terminators with it: "好了！」"
            // and "什么？！" are one ending each.
            while (i < text.size()) {
                std::size_t n2;
                const unsigned cp2 = decode(text, i, &n2);
                if (!is_closer(cp2) && !is_hard_stop(cp2)) {
                    break;
                }
                i += n2;
                ++chars;
            }
            // And the whitespace after it, so the next piece does not open with
            // a space. The bytes stay inside THIS piece, which is what keeps
            // the ranges contiguous.
            while (i < text.size()) {
                std::size_t n2;
                const unsigned cp2 = decode(text, i, &n2);
                if (!is_space(cp2)) {
                    break;
                }
                i += n2;
            }
            if (chars >= min_chars) {
                Sentence s;
                s.begin = begin;
                s.end   = i;
                s.text  = text.substr(begin, i - begin);
                out.push_back(s);
                begin = i;
                chars = 0;
                last_secondary = 0;
            }
            prev_cp = cp;
            continue;
        }

        if (chars >= max_chars) {
            // No full stop in reach. Cut at the last comma if this piece has
            // one, and at the character boundary we are standing on if it does
            // not -- a run-on with no punctuation at all still has to be cut
            // somewhere, and the seam is the price of not handing the engine an
            // unbounded utterance.
            const std::size_t cut = (last_secondary > begin) ? last_secondary : i;
            Sentence s;
            s.begin = begin;
            s.end   = cut;
            s.text  = text.substr(begin, cut - begin);
            out.push_back(s);
            begin = cut;
            // Rescan from the cut rather than carrying a corrected count
            // forward: at most max_chars characters are looked at twice, and
            // the arithmetic that would avoid it is the kind that goes wrong
            // once and then stays wrong.
            i = cut;
            chars = 0;
            last_secondary = 0;
            prev_cp = 0;
            continue;
        }

        prev_cp = cp;
    }

    if (begin < text.size()) {
        // The tail. If it is too short to stand alone, it joins the piece
        // before it rather than becoming a two-character utterance -- there is
        // nothing after it to absorb it, which is the case min_chars cannot
        // handle by waiting.
        if (chars < min_chars && !out.empty()) {
            out.back().end  = text.size();
            out.back().text = text.substr(out.back().begin, text.size() - out.back().begin);
        } else {
            Sentence s;
            s.begin = begin;
            s.end   = text.size();
            s.text  = text.substr(begin);
            out.push_back(s);
        }
    }

    // And now the last piece against its OWN minimum, which is higher: a short
    // final utterance is the one the model does not give a release to. See
    // SplitOptions::tail_chars for the four measurements.
    //
    // Backwards only, and as far as it takes. A passage with nothing long
    // enough in it becomes one piece, which is the right answer for a passage
    // that short.
    while (out.size() >= 2) {
        const Sentence& last = out.back();
        std::size_t chars_in_last = 0;
        for (std::size_t i = 0; i < last.text.size(); ) {
            std::size_t len;
            decode(last.text, i, &len);
            i += len;
            ++chars_in_last;
        }
        if (chars_in_last >= opt.tail_chars) {
            break;
        }
        const std::size_t end = last.end;
        out.pop_back();
        out.back().end  = end;
        out.back().text = text.substr(out.back().begin, end - out.back().begin);
    }
    return out;
}

//------------------------------------------------------------------------------
// Reader
//------------------------------------------------------------------------------

struct Reader::Impl {
    std::vector<Qtts*> engines;
    SplitOptions       split;
    std::string        instruct;
    std::string        speaker;
    std::string        error;
    long long          seed;
    int                threads;
    QttsSampling       sampling;

    // One sentence's worth of work. `pcm` holds what has been generated and not
    // yet handed to the sink; the emitter swaps it out rather than copying.
    struct Piece {
        std::string        text;
        std::vector<float> pcm;
        bool               done;
        Piece() : done(false) {}
    };

    // Per-read state, guarded by `mu`.
    //
    // A DEQUE, and that is load-bearing now that pieces arrive while the
    // workers are running: a worker holds a reference to pieces[job] for the
    // whole of its say(), and a vector that reallocates on push_back would
    // leave it dangling. A deque's push_back does not invalidate references to
    // the elements already in it.
    std::deque<Piece>       pieces;
    std::mutex              mu;
    std::condition_variable cv;
    std::size_t             next_job;
    std::size_t             next_emit;
    bool                    aborted;
    bool                    failed;
    // No further pieces will be added. Until this is set, a worker or the
    // emitter that has run out of work waits instead of finishing -- which is
    // the whole difference between reading a passage and reading a stream.
    bool                    closed;
    std::size_t             held;        // samples buffered and not yet emitted
    std::size_t             held_peak;
    std::size_t             emitted;     // samples handed to the sink

    // The threads, and what the stream needs to outlive a call.
    std::vector<std::thread> pool;
    std::thread              emitter;
    ReaderAudioSink          sink;
    void*                    sink_user;
    bool                     running;
    std::string              pending;    // text not yet settled into a piece
    clk::time_point          started;
    double                   first_audio;
    double                   starved;

    Impl() : seed(23), threads(0), next_job(0), next_emit(0),
             aborted(false), failed(false), closed(false),
             held(0), held_peak(0), emitted(0),
             sink(0), sink_user(0), running(false),
             first_audio(0.0), starved(0.0) {}

    ~Impl() { drop_engines(); }

    void drop_engines() {
        for (std::size_t k = 0; k < engines.size(); ++k) {
            delete engines[k];
        }
        engines.clear();
    }

    // Push every setting onto every engine.
    //
    // Called from load(), not only from the setters, because the setters are
    // allowed to run BEFORE a load -- a host reads its configuration and then
    // opens the model -- and at that point there is nothing to push them onto.
    // Without this, a voice set before loading was silently the engine's
    // default, which on a voice_design checkpoint is not a default at all: it
    // is the one state say() refuses.
    //
    // The seed is in here for a reason of its own. Left at the engine's -1 it
    // is redrawn per say(), which across a split passage means a fresh draw at
    // every sentence; see Reader::set_seed.
    void apply_settings() {
        for (std::size_t k = 0; k < engines.size(); ++k) {
            engines[k]->set_voice(instruct);
            engines[k]->set_speaker(speaker);
            engines[k]->set_seed(seed);
            engines[k]->set_sampling(sampling);
            engines[k]->set_streaming(true);   // the chunk path is what ordering runs on
        }
    }

    // What a worker hands the engine, so the chunk callback knows whose samples
    // these are. A stack local in the worker: it outlives the say() call that
    // uses it and nothing else touches it.
    struct ChunkCtx {
        Impl*       impl;
        std::size_t job;
    };

    // Runs on qwentts.cpp's compute worker. Everything it does is under the
    // lock and none of it is slow: append, and wake the emitter. The engine's
    // own documentation is explicit that a slow body here stalls generation,
    // which is the whole reason the host's sink is called from somewhere else.
    static bool on_chunk(const float* pcm, std::size_t n, bool /*last*/, void* user_data) {
        ChunkCtx* c = static_cast<ChunkCtx*>(user_data);
        Impl* m = c->impl;
        std::lock_guard<std::mutex> lock(m->mu);
        if (m->aborted || m->failed) {
            return false;
        }
        if (n > 0) {
            Piece& p = m->pieces[c->job];
            p.pcm.insert(p.pcm.end(), pcm, pcm + n);
            m->held += n;
            if (m->held > m->held_peak) {
                m->held_peak = m->held;
            }
            m->cv.notify_all();
        }
        return true;
    }

    // How far ahead of the sentence being emitted a worker may start.
    //
    // Without a bound, engines race to the end of the passage and everything
    // they produce is held until the first sentence finishes -- which on a long
    // reply is the whole reply in memory, at 96 KB per second of speech. Two
    // sentences per engine is enough to keep every engine busy while the
    // emitter drains, and caps the buffer at something a phone can hold.
    std::size_t lookahead() const {
        return engines.size() * 2;
    }

    void run_worker(std::size_t id, bool want_audio) {
        Qtts* engine = engines[id];
        for (;;) {
            std::size_t job;
            {
                std::unique_lock<std::mutex> lock(mu);
                for (;;) {
                    if (aborted || failed) {
                        return;
                    }
                    if (next_job >= pieces.size()) {
                        // Out of work. On a passage that is the end; on a
                        // stream it only means the text has not caught up.
                        if (closed) {
                            return;
                        }
                    } else if (next_job < next_emit + lookahead()) {
                        break;
                    }
                    cv.wait(lock);
                }
                job = next_job++;
            }

            ChunkCtx ctx;
            ctx.impl = this;
            ctx.job  = job;

            QttsResult r;
            const bool ok = engine->say(pieces[job].text,
                                        want_audio ? &Impl::on_chunk : 0,
                                        want_audio ? &ctx : 0, &r);

            std::lock_guard<std::mutex> lock(mu);
            pieces[job].done = true;
            if (!ok && !aborted) {
                // The first failure ends the passage. See the note on read():
                // whatever the engine refused, it will refuse for every other
                // sentence too.
                failed = true;
                if (error.empty()) {
                    error = engine->last_error();
                }
            }
            if (!want_audio) {
                emitted += static_cast<std::size_t>(r.speech_seconds * Qtts::sample_rate());
            }
            cv.notify_all();
        }
    }

    // Drains the pieces in order on the calling thread. Returns the silence a
    // player would have heard.
    //
    // **Not the time this thread spent waiting**, which was the first thing
    // written here and measures nothing: a sink that copies and returns drains
    // every handover instantly, so this thread idles between chunks whatever
    // the engine is doing, and the figure came out equal to the whole run at
    // RTF 0.59 and at RTF 1.30 alike.
    //
    // What a listener experiences needs a playback clock instead. `playhead` is
    // the wall-clock instant at which everything handed over so far would
    // finish playing; audio arriving after it is a hole of exactly that length,
    // and audio arriving before it simply queues.
    // Whether the piece just finished was the last one. Read under the lock by
    // the caller's contract: only the emitter moves next_emit, and only it
    // calls this.
    // Whether the piece just finished was the last one there will ever be.
    // On a stream that needs `closed` as well as the count: a piece that is
    // currently last is not the final one if more text is still coming, and
    // ending the passage on it would fade a join.
    bool next_emit_is_past_end() {
        std::lock_guard<std::mutex> lock(mu);
        return closed && next_emit >= pieces.size();
    }

    // Cut whatever text has arrived into pieces the workers can claim.
    void drain_pending(bool closing);

    // Whether a piece can be dispatched without waiting for what follows it.
    bool settled_at_end(const std::string& text) const;

    // run_emitter on a thread of its own, keeping its two results where
    // finish() can read them after the join.
    void stream_emitter() {
        starved = run_emitter(sink, sink_user, &first_audio, started);
    }

    double run_emitter(ReaderAudioSink sink, void* user_data, double* first_audio,
                       clk::time_point started) {
        std::vector<float> batch;
        std::vector<float> shaped;
        double starved  = 0.0;
        double playhead = 0.0;
        bool   opened   = false;

        // One shaper for the whole passage, reset at each piece by finish().
        // It is here rather than in the worker because trimming a join needs
        // both sides of it, and this is the only place where the pieces are
        // seen in order.
        EdgeShaper shaper(Qtts::sample_rate());

        for (;;) {
            std::size_t idx;
            bool finished = false;
            {
                std::unique_lock<std::mutex> lock(mu);
                for (;;) {
                    if (aborted || failed) {
                        return starved;
                    }
                    if (next_emit >= pieces.size()) {
                        if (closed) {
                            return starved;
                        }
                    } else if (!pieces[next_emit].pcm.empty() || pieces[next_emit].done) {
                        break;
                    }
                    cv.wait(lock);
                }
                idx = next_emit;
                batch.clear();
                batch.swap(pieces[idx].pcm);
                held -= batch.size();
                finished = pieces[idx].done;
                if (finished) {
                    ++next_emit;
                }
                // A worker may now be allowed to start a sentence that was
                // beyond the lookahead.
                cv.notify_all();
            }

            shaped.clear();
            if (!batch.empty()) {
                shaper.feed(batch, &shaped);
            }
            if (finished) {
                // next_emit was advanced above, so this compares against the
                // count rather than against idx + 1.
                shaper.finish(next_emit_is_past_end(), &shaped);
            }

            if (!shaped.empty()) {
                const double now = seconds_since(started);
                if (!opened) {
                    opened       = true;
                    *first_audio = now;
                    playhead     = now;   // the listener starts here, not at 0
                } else if (now > playhead) {
                    starved += now - playhead;
                    playhead = now;
                }
                playhead += static_cast<double>(shaped.size()) / Qtts::sample_rate();
                emitted += shaped.size();
                if (sink && !sink(&shaped[0], shaped.size(), static_cast<int>(idx), false,
                                  user_data)) {
                    std::lock_guard<std::mutex> lock(mu);
                    aborted = true;
                    cv.notify_all();
                    return starved;
                }
            }
        }
    }
};

Reader::Reader() : impl_(new Impl()) {}

Reader::~Reader() {
    delete impl_;
}

bool Reader::available() {
    return Qtts::available();
}

int Reader::sample_rate() {
    return Qtts::sample_rate();
}

bool Reader::load(const std::string& talker_path, const std::string& codec_path, int voices) {
    Impl* m = impl_;
    m->error.clear();
    m->drop_engines();

    if (voices < 1) {
        voices = 1;
    }

    const clk::time_point started = clk::now();
    for (int k = 0; k < voices; ++k) {
        Qtts* engine = new Qtts();
        engine->set_threads(m->threads);
        if (!engine->load(talker_path, codec_path)) {
            m->error = engine->last_error();
            delete engine;
            // All or nothing: a half-loaded pool would read some sentences and
            // silently skip the rest, which is worse than not starting.
            m->drop_engines();
            platform::log_error("reader: voice %d of %d failed to load -- %s",
                                k + 1, voices, m->error.c_str());
            return false;
        }
        m->engines.push_back(engine);
    }

    m->apply_settings();
    platform::log_info("reader: %d voice(s) loaded in %.1f s, %s",
                       voices, seconds_since(started), m->engines[0]->model_type().c_str());
    return true;
}

bool Reader::loaded() const {
    return !impl_->engines.empty();
}

void Reader::unload() {
    impl_->drop_engines();
}

int Reader::voices() const {
    return static_cast<int>(impl_->engines.size());
}

std::string Reader::model_type() const {
    return impl_->engines.empty() ? std::string() : impl_->engines[0]->model_type();
}

std::vector<std::string> Reader::speakers() const {
    return impl_->engines.empty() ? std::vector<std::string>() : impl_->engines[0]->speakers();
}

void Reader::set_voice(const std::string& instruct) {
    impl_->instruct = instruct;
    for (std::size_t k = 0; k < impl_->engines.size(); ++k) {
        impl_->engines[k]->set_voice(instruct);
    }
}

void Reader::set_speaker(const std::string& name) {
    impl_->speaker = name;
    for (std::size_t k = 0; k < impl_->engines.size(); ++k) {
        impl_->engines[k]->set_speaker(name);
    }
}

bool Reader::set_reference_voice(const std::string& wav_path) {
    Impl* m = impl_;
    m->error.clear();
    if (m->engines.empty()) {
        m->error = "set_reference_voice needs a loaded model";
        return false;
    }
    // Extracted once per engine rather than once and copied, because the
    // extraction runs each engine's own speaker encoder and the embedding is
    // only meaningful to the model that produced it. With one voice, which is
    // what the measurements say to use, that is one extraction.
    for (std::size_t k = 0; k < m->engines.size(); ++k) {
        if (!m->engines[k]->set_reference_voice(wav_path)) {
            m->error = m->engines[k]->last_error();
            return false;
        }
    }
    return true;
}

void Reader::set_seed(long long seed) {
    impl_->seed = seed;
    for (std::size_t k = 0; k < impl_->engines.size(); ++k) {
        impl_->engines[k]->set_seed(seed);
    }
}

void Reader::set_sampling(const QttsSampling& sampling) {
    impl_->sampling = sampling;
    for (std::size_t k = 0; k < impl_->engines.size(); ++k) {
        impl_->engines[k]->set_sampling(sampling);
    }
}

const QttsSampling& Reader::sampling() const {
    return impl_->sampling;
}

void Reader::set_threads(int n) {
    impl_->threads = n;
}

void Reader::set_split(const SplitOptions& opt) {
    impl_->split = opt;
}

const SplitOptions& Reader::split() const {
    return impl_->split;
}

const std::string& Reader::last_error() const {
    return impl_->error;
}

// Dispatch whatever of `pending` is now settled.
//
// A piece is settled when no further text could change where it ends, and a
// piece handed to an engine cannot be taken back.
//
// **Not simply "all but the last one"**, which is what this did first and what
// cost most of a reply's latency. A piece that already ends at a full stop and
// is long enough to stand alone is finished whatever arrives next -- the
// splitter would cut it in the same place either way -- so holding it back
// bought nothing and waited for the NEXT sentence to begin. Measured on a
// two-sentence reply, that is what delayed the first word until 605 ms before
// the writer had finished entirely.
//
// The one that genuinely cannot be settled is a piece still mid-sentence: more
// text extends it, or a full stop three characters later moves its end.
bool Reader::Impl::settled_at_end(const std::string& text) const {
    const std::string body = trimmed(text);
    if (body.empty() || chars_in(body) < split.min_chars) {
        return false;
    }
    // The last character, which needs the start of its own sequence found.
    std::size_t start = body.size() - 1;
    while (start > 0 && (static_cast<unsigned char>(body[start]) & 0xC0) == 0x80) {
        --start;
    }
    std::size_t len;
    const unsigned cp = decode(body, start, &len);
    if (is_hard_stop(cp) || is_closer(cp)) {
        return true;
    }
    // With clause cuts on, a comma ends a piece as firmly as a full stop does,
    // so a fragment ending at one is not going to move either.
    return split.clause_cuts && is_secondary(cp);
}

void Reader::Impl::drain_pending(bool closing) {
    if (pending.empty()) {
        return;
    }
    // **The tail rule is off until the text is finished**, and leaving it on was
    // the second thing that kept a stream silent. It exists to stop the LAST
    // piece of a reply being short, because a short final utterance is the one
    // the engine ends without a release -- but mid-stream the trailing fragment
    // is not the last piece of anything, it is the sentence still being
    // written. Applied to it, the rule merged it backwards on every drain, so
    // the piece in front of it was never separated out and never dispatched
    // until the fragment happened to grow past tail_chars.
    //
    // Measured on the same passage through both doors: handed over whole it cut
    // 39 + 47 characters, streamed it cut 62 + 16.
    SplitOptions opt = split;
    if (!closing) {
        opt.tail_chars = 0;
    }
    const std::vector<Sentence> cut = split_sentences(pending, opt);
    std::size_t settled = closing ? cut.size() : (cut.size() > 1 ? cut.size() - 1 : 0);
    if (!closing && !cut.empty() && settled + 1 == cut.size() &&
        settled_at_end(cut.back().text)) {
        ++settled;
    }
    if (settled == 0) {
        return;
    }

    std::size_t consumed = 0;
    std::lock_guard<std::mutex> lock(mu);
    for (std::size_t k = 0; k < settled; ++k) {
        consumed = cut[k].end;
        const std::string spoken = trimmed(cut[k].text);
        if (spoken.empty()) {
            continue;   // whitespace between sentences; nothing to say
        }
        // The tail rule cannot reach backwards past a piece an engine already
        // has. It still can while the last one is only queued, which is the
        // common case at the end of a reply: the text finishes faster than the
        // speech does. See SplitOptions::tail_chars.
        const bool short_tail = closing && k + 1 == settled &&
                                chars_in(spoken) < opt.tail_chars;
        if (short_tail && !pieces.empty() && next_job < pieces.size()) {
            pieces.back().text += spoken;
            continue;
        }
        Piece piece;
        piece.text = spoken;
        pieces.push_back(piece);
        // The exact string an engine is about to be given, which is NOT the
        // reply on screen: a host may strip markup before feeding it, and the
        // difference is invisible from the outside. A piece that reads its own
        // asterisks aloud, or runs away generating, looks like a synthesis
        // fault until the text that caused it can be seen.
        //
        // This used to come out as mojibake on a Windows console, which is a
        // code page rendering correct UTF-8 bytes rather than anything wrong
        // with the text. platform/log.cpp now writes a record through
        // WriteConsoleW when stderr is a console, so the code page is out of
        // the path and `chcp 65001` is no longer needed to read this line.
        platform::log_info("reader: piece %d, %d char(s): %s",
                           static_cast<int>(pieces.size()) - 1,
                           static_cast<int>(chars_in(spoken)), spoken.c_str());
    }
    pending.erase(0, consumed);
    cv.notify_all();
}

bool Reader::begin(ReaderAudioSink sink, void* user_data) {
    Impl* m = impl_;
    m->error.clear();
    if (m->engines.empty()) {
        m->error = "no speech model loaded";
        return false;
    }
    if (m->running) {
        m->error = "already reading";
        return false;
    }

    m->pieces.clear();
    m->pending.clear();
    m->next_job    = 0;
    m->next_emit   = 0;
    m->aborted     = false;
    m->failed      = false;
    m->closed      = false;
    m->held        = 0;
    m->held_peak   = 0;
    m->emitted     = 0;
    m->first_audio = 0.0;
    m->starved     = 0.0;
    m->sink        = sink;
    m->sink_user   = user_data;
    m->started     = clk::now();
    m->running     = true;

    // Every engine gets a worker, not one per piece: on a stream the piece
    // count is not known yet, and a worker with nothing to do waits.
    const bool want_audio = (sink != 0);
    m->pool.reserve(m->engines.size());
    for (std::size_t k = 0; k < m->engines.size(); ++k) {
        m->pool.push_back(std::thread(&Impl::run_worker, m, k, want_audio));
    }
    if (want_audio) {
        // On its own thread here, unlike read(), because begin() returns and
        // there is no caller left to run it on.
        m->emitter = std::thread(&Impl::stream_emitter, m);
    }
    return true;
}

bool Reader::feed(const std::string& delta) {
    Impl* m = impl_;
    if (!m->running) {
        m->error = "feed without begin";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m->mu);
        if (m->aborted || m->failed) {
            return false;
        }
    }
    m->pending += delta;
    m->drain_pending(false);
    return true;
}

bool Reader::finish(ReaderResult* out) {
    Impl* m = impl_;
    if (out) {
        *out = ReaderResult();
    }
    if (!m->running) {
        m->error = "finish without begin";
        return false;
    }

    m->drain_pending(true);
    {
        std::lock_guard<std::mutex> lock(m->mu);
        m->closed = true;
        m->cv.notify_all();
    }

    for (std::size_t k = 0; k < m->pool.size(); ++k) {
        m->pool[k].join();
    }
    m->pool.clear();
    if (m->emitter.joinable()) {
        m->emitter.join();
    }
    m->running = false;

    const double elapsed = seconds_since(m->started);
    const bool want_audio = (m->sink != 0);

    if (m->failed) {
        if (m->error.empty()) {
            m->error = "a sentence failed to synthesize";
        }
        platform::log_error("reader: stopped after %.2f s -- %s", elapsed, m->error.c_str());
        return false;
    }

    if (want_audio && !m->aborted && !m->pieces.empty()) {
        m->sink(0, 0, static_cast<int>(m->pieces.size()) - 1, true, m->sink_user);
    } else if (want_audio && !m->aborted) {
        m->sink(0, 0, 0, true, m->sink_user);
    }

    const float speech = static_cast<float>(m->emitted) / static_cast<float>(sample_rate());
    if (out) {
        out->sentences           = static_cast<int>(m->pieces.size());
        out->speech_seconds      = speech;
        out->elapsed             = elapsed;
        out->first_audio_seconds = m->first_audio;
        out->starved_seconds     = m->starved;
        out->held_seconds =
            static_cast<float>(m->held_peak) / static_cast<float>(sample_rate());
    }

    // NOT the engine's ratio, and the difference matters now that text can
    // arrive slowly: `elapsed` runs from begin(), so on a stream it contains
    // however long the writer took as well as the synthesis. Measured on one
    // turn, the engine reported 0.67x for the same audio this line called 2.01x
    // -- the gap was the language model, not the speech.
    platform::log_info("reader: %d piece(s) on %d voice(s) -- %.2f s of speech in %.2f s "
                       "(%.2fx of wall clock, text included), first audio %.2f s, "
                       "%.2f s starved, %.2f s held",
                       static_cast<int>(m->pieces.size()), static_cast<int>(m->engines.size()),
                       speech, elapsed, speech > 0.0f ? elapsed / speech : 0.0,
                       m->first_audio, m->starved,
                       static_cast<double>(m->held_peak) / sample_rate());
    return true;
}

// The whole passage at once, which is now the streaming path with the text
// handed over in one go. One implementation, so a fix to either reaches both.
bool Reader::read(const std::string& text, ReaderAudioSink sink, void* user_data,
                  ReaderResult* out) {
    if (!begin(sink, user_data)) {
        return false;
    }
    feed(text);
    return finish(out);
}

}  // namespace voice
}  // namespace bodyclock
