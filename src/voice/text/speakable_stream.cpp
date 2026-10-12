#include "voice/text/speakable_stream.hpp"

namespace yoai {
namespace voice {

namespace {

// Bytes in the UTF-8 sequence starting with `lead`; 1 for anything malformed.
std::size_t utf8_len(unsigned char lead) {
    if (lead < 0x80)           return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

// Ends a sentence already, so a line break after it adds nothing: a second
// full stop straight after the first is read as a hesitation -- heard as an
// "呃" before the next sentence.
bool is_stop(unsigned cp) {
    switch (cp) {
    case 0x3002:   // 。
    case 0xFF01:   // ！
    case 0xFF1F:   // ？
    case 0xFF1B:   // ；
    case 0x2026:   // …
    case '.': case '!': case '?': case ';':
        return true;
    default:
        return false;
    }
}

// A pause inside a sentence. Held until the next character: before a line
// break it is REPLACED by the full stop, rather than followed by one.
bool is_soft_pause(unsigned cp) {
    return cp == 0xFF0C    // ，
        || cp == 0xFF1A    // ：
        || cp == 0x3001    // 、
        || cp == ',' || cp == ':';
}

// Already a pause, so a table's pipe after it needs nothing added.
bool is_pause(unsigned cp) {
    switch (cp) {
    case 0xFF0C:   // ，
    case 0x3002:   // 。
    case 0xFF01:   // ！
    case 0xFF1F:   // ？
    case 0xFF1B:   // ；
    case 0xFF1A:   // ：
    case 0x3001:   // 、
    case 0x2026:   // …
    case ',': case '.': case '!': case '?': case ';': case ':':
        return true;
    default:
        return false;
    }
}

// Closes a quote or a bracket. Skipped when deciding whether a pause came last,
// so "“好。”" counts as ending in a full stop and "（上海）" as ending in 海.
bool is_closer(unsigned cp) {
    switch (cp) {
    case 0x201D: case 0x2019:   // ” ’
    case 0x300D: case 0x300F:   // 」 』
    case 0x300B: case 0x3011:   // 》 】
    case 0x3015: case 0xFF09:   // 〕 ）
    case ')': case ']': case '"': case '\'':
        return true;
    default:
        return false;
    }
}

// What may open a line as markup. Held at a line start until the line's first
// other character shows whether it was.
bool is_line_mark(unsigned cp) {
    switch (cp) {
    case ' ': case '\t':
    case '-': case '*': case '+': case '>': case '#':
    case '`': case '|': case ':': case '_': case '=': case '~':
        return true;
    default:
        return false;
    }
}

// Emoji, and the characters that join or modify them.
//
// An approximation of Unicode's Extended_Pictographic, which is the property
// the desktop used, by block rather than by the property's own table. It is the
// emoji core without the ASCII digits, `#` and `*` that the wider Emoji property
// also matches -- a reply saying "14 tok/s" keeps its number. A keycap's digit
// is left behind for the same reason.
bool is_emoji(unsigned cp) {
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return true;   // pictographs, skin tones, flags
    if (cp >= 0x2600 && cp <= 0x27BF)   return true;   // symbols and dingbats
    if (cp >= 0xE0020 && cp <= 0xE007F) return true;   // tag sequences
    if (cp >= 0x2194 && cp <= 0x2199)   return true;
    if (cp >= 0x23E9 && cp <= 0x23F3)   return true;
    if (cp >= 0x23F8 && cp <= 0x23FA)   return true;
    if (cp >= 0x25FB && cp <= 0x25FE)   return true;
    if (cp >= 0x2B05 && cp <= 0x2B07)   return true;
    switch (cp) {
    case 0x00A9: case 0x00AE: case 0x203C: case 0x2049: case 0x2122: case 0x2139:
    case 0x21A9: case 0x21AA: case 0x231A: case 0x231B: case 0x2328: case 0x23CF:
    case 0x24C2: case 0x25AA: case 0x25AB: case 0x25B6: case 0x25C0:
    case 0x2934: case 0x2935: case 0x2B1B: case 0x2B1C: case 0x2B50: case 0x2B55:
    case 0x3030: case 0x303D: case 0x3297: case 0x3299:
    case 0xFE0E: case 0xFE0F:   // variation selectors
    case 0x200D:                // zero-width joiner
    case 0x20E3:                // enclosing keycap
        return true;
    default:
        return false;
    }
}

// Longest link label held while waiting for its "]". Past it, the "[" was not a
// link, and the text is put back as written.
const std::size_t kMaxLabel = 300;

// Longest run of marks held at a line start.
const std::size_t kMaxPrefix = 64;

}  // namespace

SpeakableStream::SpeakableStream() {
    reset();
}

void SpeakableStream::reset() {
    out_.clear();
    partial_.clear();
    prefix_.clear();
    label_.clear();
    url_bytes_  = 0;
    link_       = kNone;
    hold_       = 0;
    line_start_ = true;
    drop_line_  = false;
    space_owed_ = false;
    pipe_owed_  = false;
    replaying_  = false;
    emitted_    = false;
    line_text_  = false;
    soft_.clear();
    last_       = 0;
}

std::string SpeakableStream::feed(const std::string& delta) {
    std::string in;
    in.swap(partial_);
    in += delta;

    std::size_t i = 0;
    while (i < in.size()) {
        const unsigned char lead = static_cast<unsigned char>(in[i]);
        std::size_t n = utf8_len(lead);
        if (i + n > in.size()) {
            // The rest of this character is in the next delta.
            partial_.assign(in, i, std::string::npos);
            break;
        }
        unsigned cp = lead;
        if (n > 1) {
            static const unsigned kLeadMask[5] = {0, 0, 0x1F, 0x0F, 0x07};
            cp = lead & kLeadMask[n];
            for (std::size_t k = 1; k < n; ++k) {
                const unsigned char c = static_cast<unsigned char>(in[i + k]);
                if ((c & 0xC0) != 0x80) {
                    // Malformed: the lead byte goes through as an ordinary
                    // character, and the next byte starts afresh.
                    n  = 1;
                    cp = 0xFFFD;
                    break;
                }
                cp = (cp << 6) | (c & 0x3F);
            }
        }
        on_codepoint(cp, &in[i], n);
        i += n;
    }

    std::string ready;
    ready.swap(out_);
    return ready;
}

std::string SpeakableStream::finish() {
    partial_.clear();   // a sequence that never completed is not a character
    if (line_start_) {
        prefix_.clear();   // the last line was marks only
    }
    pipe_owed_ = false;
    abort_link();
    flush_hold();
    out_ += soft_;   // the reply ends on it; nothing follows to replace it for
    soft_.clear();

    std::string ready;
    ready.swap(out_);
    reset();
    return ready;
}

void SpeakableStream::on_codepoint(unsigned cp, const char* bytes, std::size_t len) {
    if (cp == '\r') {
        return;
    }
    if (cp == '\n') {
        line_end();
        return;
    }
    if (drop_line_) {
        return;
    }
    if (line_start_) {
        if (is_line_mark(cp) && prefix_.size() < kMaxPrefix) {
            prefix_ += static_cast<char>(cp);
            return;
        }
        resolve_prefix();
        if (drop_line_) {
            return;
        }
    }
    inline_codepoint(cp, bytes, len);
}

// The marks a line began with, now that something else has followed them.
void SpeakableStream::resolve_prefix() {
    line_start_ = false;
    std::string s;
    s.swap(prefix_);

    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    if (s.compare(i, 3, "```") == 0 || s.compare(i, 3, "~~~") == 0) {
        drop_line_ = true;   // a code fence, and its language name after it
        return;
    }

    // Openers, possibly nested: "> - " is a list item inside a quote. A bullet
    // or a heading needs a space after it -- "-5 度" and "#话题" are words.
    for (;;) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
            ++i;
        }
        if (i + 1 < s.size() && (s[i] == '-' || s[i] == '*' || s[i] == '+') &&
            (s[i + 1] == ' ' || s[i + 1] == '\t')) {
            i += 2;
            continue;
        }
        if (i < s.size() && s[i] == '>') {
            ++i;
            continue;
        }
        if (i < s.size() && s[i] == '#') {
            std::size_t j = i;
            while (j < s.size() && s[j] == '#') {
                ++j;
            }
            if (j - i <= 6 && j < s.size() && (s[j] == ' ' || s[j] == '\t')) {
                i = j;
                continue;
            }
        }
        break;
    }

    // What is left is ordinary text that happens to be made of marks: the "**"
    // of emphasis, a "|" opening a table row, the "-" of "-5".
    for (; i < s.size(); ++i) {
        inline_codepoint(static_cast<unsigned char>(s[i]), &s[i], 1);
    }
}

void SpeakableStream::inline_codepoint(unsigned cp, const char* bytes, std::size_t len) {
    switch (link_) {
    case kLabel:
        if (cp == ']') {
            link_ = kAfterLabel;
            return;
        }
        if (label_.size() + len <= kMaxLabel) {
            label_.append(bytes, len);
            return;
        }
        abort_link();
        break;   // and this character is read as it comes
    case kAfterLabel:
        if (cp == '(') {
            link_      = kUrl;
            url_bytes_ = 0;
            return;
        }
        abort_link();   // "[x]" with no URL: put back as written
        break;
    case kUrl:
        if (cp == ')') {
            std::string label;
            label.swap(label_);
            link_ = kNone;
            replay(label);
            return;
        }
        url_bytes_ += len;
        return;
    case kNone:
        break;
    }

    if (hold_ != 0) {
        const char h = hold_;
        hold_ = 0;
        if ((h == '_' || h == '~') && cp == static_cast<unsigned char>(h)) {
            return;   // "__" or "~~": markup
        }
        if (h == '!' && cp == '[') {
            link_ = kLabel;   // an image, which keeps its alt text
            label_.clear();
            return;
        }
        put_ascii(h);
    }

    switch (cp) {
    case '*':
    case '`':
        return;
    case '_':
    case '~':
        hold_ = static_cast<char>(cp);
        return;
    case '!':
        if (!replaying_) {
            hold_ = '!';
            return;
        }
        break;
    case '[':
        if (!replaying_) {
            link_ = kLabel;
            label_.clear();
            return;
        }
        break;
    case '|':
        pipe_owed_  = true;
        space_owed_ = false;
        return;
    case ' ':
    case '\t':
    case 0x3000:
        if (!pipe_owed_) {
            space_owed_ = emitted_;
        }
        return;
    default:
        break;
    }
    if (is_emoji(cp)) {
        return;
    }
    put(cp, bytes, len);
}

void SpeakableStream::line_end() {
    if (line_start_) {
        prefix_.clear();   // nothing but marks: a rule, a separator row, a fence
    }
    pipe_owed_ = false;    // a row's closing "|" is not a pause of its own
    if (!drop_line_) {
        abort_link();      // a link does not run across lines
        flush_hold();
    }
    drop_line_  = false;
    line_start_ = true;
    // A blank line, or one of marks only, has nothing to end -- and leaves the
    // space owed by the line before it alone.
    if (!line_text_) {
        return;
    }
    line_text_ = false;
    // A line ends a sentence, and the cutter needs a hard stop there to cut at
    // -- "包括：" at the end of a line is only a colon to it otherwise. So a
    // comma or colon ending the line becomes the full stop, and a full stop
    // already there stays alone: two in a row are read as a hesitation.
    const bool replacing = !soft_.empty();
    soft_.clear();
    if (replacing || !is_stop(last_)) {
        if (last_ < 0x80) {
            out_ += '.';
            last_ = '.';
        } else {
            out_ += "\xE3\x80\x82";   // 。
            last_ = 0x3002;
        }
    }
    // Two ASCII words either side of the break still need a space between them.
    space_owed_ = (last_ < 0x80);
}

void SpeakableStream::abort_link() {
    if (link_ == kNone) {
        return;
    }
    std::string label;
    label.swap(label_);
    const Link was = link_;
    link_ = kNone;
    if (was == kUrl) {
        replay(label);   // the URL never closed; the label is still the words
    } else if (was == kAfterLabel) {
        replay("[" + label + "]");
    } else {
        replay("[" + label);
    }
}

void SpeakableStream::flush_hold() {
    if (hold_ != 0) {
        const char h = hold_;
        hold_ = 0;
        put_ascii(h);
    }
}

// Text put back through the inline rules, with "[" and "!" taken literally so
// it cannot open another link.
void SpeakableStream::replay(const std::string& text) {
    const bool was = replaying_;
    replaying_ = true;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t n = utf8_len(lead);
        if (i + n > text.size()) {
            n = 1;
        }
        unsigned cp = lead;
        if (n > 1) {
            static const unsigned kLeadMask[5] = {0, 0, 0x1F, 0x0F, 0x07};
            cp = lead & kLeadMask[n];
            for (std::size_t k = 1; k < n; ++k) {
                cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            }
        }
        inline_codepoint(cp, &text[i], n);
        i += n;
    }
    flush_hold();
    replaying_ = was;
}

void SpeakableStream::put(unsigned cp, const char* bytes, std::size_t len) {
    // Something follows the held comma or colon on the same line, so it was a
    // pause inside the sentence after all.
    out_ += soft_;
    soft_.clear();
    if (pipe_owed_) {
        pipe_owed_  = false;
        space_owed_ = false;
        if (emitted_ && !is_pause(last_)) {
            if (last_ < 0x80) {
                out_ += ", ";
                last_ = ',';
            } else {
                out_ += "\xEF\xBC\x8C";   // ，
                last_ = 0xFF0C;
            }
        } else if (emitted_ && last_ < 0x80) {
            out_ += ' ';
        }
    }
    if (space_owed_) {
        space_owed_ = false;
        if (emitted_) {
            out_ += ' ';
        }
    }
    if (is_soft_pause(cp)) {
        soft_.assign(bytes, len);   // see line_end()
    } else {
        out_.append(bytes, len);
    }
    emitted_   = true;
    line_text_ = true;
    if (!is_closer(cp)) {
        last_ = cp;
    }
}

void SpeakableStream::put_ascii(char c) {
    put(static_cast<unsigned char>(c), &c, 1);
}

std::string speakable(const std::string& text) {
    SpeakableStream s;
    std::string out = s.feed(text);
    out += s.finish();
    return out;
}

}  // namespace voice
}  // namespace yoai
