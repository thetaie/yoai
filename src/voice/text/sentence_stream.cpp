#include "voice/text/sentence_stream.hpp"

namespace yoai {
namespace voice {

namespace {

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

// ASCII whitespace plus the ideographic space, from both ends. What reaches the
// consumer, never what the Sentence records.
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

}  // namespace

std::size_t count_chars(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ) {
        std::size_t len;
        decode(s, i, &len);
        i += len;
        ++n;
    }
    return n;
}

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
        const bool stop = terminates_here(text, cur, cp, len, prev_cp);
        // How long a piece must be to end here: shorter at a full stop when
        // SplitOptions::stop_chars says so.
        const std::size_t need =
            (stop && opt.stop_chars > 0 && opt.stop_chars < min_chars) ? opt.stop_chars : min_chars;
        if (stop || clause || (newline && chars >= min_chars)) {
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
            if (chars >= need) {
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
        if (count_chars(last.text) >= opt.tail_chars) {
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
// Streaming
//------------------------------------------------------------------------------

SentenceStream::SentenceStream(const SplitOptions& opt) : opt_(opt) {}

void SentenceStream::reset(const SplitOptions& opt) {
    opt_ = opt;
    pending_.clear();
    pieces_out_ = 0;
    urgent_     = false;
}

// The rules for the piece being cut now: the first piece's for the first one
// and for an urgent one, the later pieces' otherwise.
SplitOptions SentenceStream::current() const {
    SplitOptions o = opt_;
    if (pieces_out_ == 0 || urgent_) {
        return o;   // the first piece's rules, stop_chars included
    }
    // The pieces between wait for a whole sentence of their own; stop_chars is
    // for the one the listener is waiting on.
    o.stop_chars = 0;
    if (opt_.later_min_chars == 0) {
        return o;
    }
    o.min_chars   = opt_.later_min_chars;
    o.max_chars   = opt_.later_max_chars > o.min_chars ? opt_.later_max_chars : o.min_chars;
    o.clause_cuts = false;
    return o;
}

std::vector<std::string> SentenceStream::feed(const std::string& delta) {
    pending_ += delta;
    return drain(false);
}

std::vector<std::string> SentenceStream::finish() {
    return drain(true);
}

// Whether a piece can be handed over without waiting for what follows it.
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
bool SentenceStream::settled_at_end(const std::string& text, const SplitOptions& opt) const {
    const std::string body = trimmed(text);
    if (body.empty()) {
        return false;
    }
    // The shortest a settled piece can be: stop_chars when it ends at a full
    // stop, which is checked below, and min_chars otherwise.
    const std::size_t n = count_chars(body);
    const std::size_t shortest =
        (opt.stop_chars > 0 && opt.stop_chars < opt.min_chars) ? opt.stop_chars : opt.min_chars;
    if (n < shortest) {
        return false;
    }
    // The last character that is not a closing quote or bracket. A closer ends
    // a sentence only when a terminator comes before it -- "他说：“去。”" -- and
    // mid-sentence it closes a title or a quotation and the sentence goes on:
    // "也是《白蛇传》中“白娘子和许仙相遇”的地方". Counted as an ending, that one
    // was handed over at the ”, and "的地方（断桥）。" became a piece of its own.
    std::size_t end = body.size();
    unsigned cp = 0;
    while (end > 0) {
        std::size_t start = end - 1;
        while (start > 0 && (static_cast<unsigned char>(body[start]) & 0xC0) == 0x80) {
            --start;
        }
        std::size_t len;
        cp = decode(body, start, &len);
        if (!is_closer(cp)) {
            break;
        }
        end = start;
        cp  = 0;
    }
    if (is_hard_stop(cp)) {
        return true;
    }
    // Below min_chars only a full stop settles a piece.
    if (n < opt.min_chars) {
        return false;
    }
    // With clause cuts on, a comma ends a piece as firmly as a full stop does,
    // so a fragment ending at one is not going to move either.
    return opt.clause_cuts && is_secondary(cp);
}

std::vector<std::string> SentenceStream::drain(bool closing) {
    std::vector<std::string> out;
    // Round by round, because the rules can change after ONE piece: the first
    // piece, or an urgent one, is cut by min_chars and the ones after it by
    // later_min_chars. Cutting all the pending text in one go would cut the
    // whole of it by whichever rules held at the start. When the rules cannot
    // change this is a single round, exactly as it was.
    while (!pending_.empty()) {
        const SplitOptions rules = current();
        const bool one_piece =
            (opt_.later_min_chars != 0 || opt_.stop_chars != 0) && (pieces_out_ == 0 || urgent_);

        // **The tail rule is off until the text is finished**, and leaving it
        // on was the second thing that kept a stream silent. It exists to stop
        // the LAST piece of a reply being short, because a short final
        // utterance is the one the engine ends without a release -- but
        // mid-stream the trailing fragment is not the last piece of anything,
        // it is the sentence still being written. Applied to it, the rule
        // merged it backwards on every drain, so the piece in front of it was
        // never separated out and never dispatched until the fragment happened
        // to grow past tail_chars.
        //
        // Measured on the same passage through both doors: handed over whole
        // it cut 39 + 47 characters, streamed it cut 62 + 16.
        //
        // Off as well for a round that takes one piece: the rest of the text is
        // cut in the next round, and that is where the tail is.
        SplitOptions opt = rules;
        const bool final_round = closing && !one_piece;
        if (!final_round) {
            opt.tail_chars = 0;
        }
        const std::vector<Sentence> cut = split_sentences(pending_, opt);
        std::size_t settled = final_round ? cut.size() : (cut.size() > 1 ? cut.size() - 1 : 0);
        if (!final_round && !cut.empty() && settled + 1 == cut.size() &&
            (closing || settled_at_end(cut.back().text, rules))) {
            ++settled;
        }
        if (settled == 0) {
            break;
        }
        if (one_piece) {
            settled = 1;
        }

        std::size_t consumed = 0;
        for (std::size_t k = 0; k < settled; ++k) {
            consumed = cut[k].end;
            std::string spoken = trimmed(cut[k].text);
            if (!spoken.empty()) {   // whitespace between sentences is dropped
                out.push_back(spoken);
                ++pieces_out_;
                urgent_ = false;   // one piece's worth; see set_urgent()
            }
        }
        pending_.erase(0, consumed);
        if (!one_piece) {
            break;
        }
    }
    return out;
}

}  // namespace voice
}  // namespace yoai
