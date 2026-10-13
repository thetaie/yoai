#include "voice/text/turn_cue.hpp"

#include <cstddef>
#include <cstring>
#include <string>

namespace yoai {
namespace voice {

namespace {

// The words a cue is looked for in: no punctuation, ASCII lowercased, runs of
// whitespace one space, none at either end. Recognition punctuates by the pause
// it heard, so "我想问一下，" and "我想问一下。" are the same words.
//
// Dropped: ASCII punctuation, U+00B7, General Punctuation (U+2010-U+206F: the
// dashes, the ellipsis, curly quotes), CJK Symbols and Punctuation
// (U+3000-U+303F) and the punctuation half of Halfwidth and Fullwidth Forms.
std::string normalize(const std::string& text) {
    std::string out;
    bool space = false;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        unsigned cp = c;
        if (c >= 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else if (c >= 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xC0) {
            len = 2;
            cp = c & 0x1F;
        }
        if (i + len > text.size()) {
            break;  // a truncated sequence at the end: nothing to read in it
        }
        for (std::size_t k = 1; k < len; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }

        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x3000) {
            space = !out.empty();
        } else if ((cp < 0x80 && std::strchr("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~", static_cast<int>(cp)) != nullptr) ||
                   cp == 0xB7 || (cp >= 0x2010 && cp <= 0x206F) ||
                   (cp >= 0x3001 && cp <= 0x303F) || (cp >= 0xFF01 && cp <= 0xFF0F) ||
                   (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) ||
                   (cp >= 0xFF5B && cp <= 0xFF65)) {
            // Punctuation separates English words as a space would.
            space = !out.empty();
        } else {
            if (space) {
                out += ' ';
                space = false;
            }
            if (cp < 0x80) {
                out += static_cast<char>(cp >= 'A' && cp <= 'Z' ? cp - 'A' + 'a' : cp);
            } else {
                out.append(text, i, len);
            }
        }
        i += len;
    }
    return out;
}

// Whether a cue only counts as a word of its own, with the end of the text or
// a pause (a space, after normalize()) beside it. English, so "so" ends "is it
// so" and not "also"; and a single Chinese character, so "停" starts "停，别说了"
// and not "停车场在哪", "不" starts "不，是杭州" and not "不过", and "额" ends
// "去北京，额" and not "退款金额".
bool needs_boundary(const char* cue) {
    const std::size_t n = std::strlen(cue);
    if (n == 3 && static_cast<unsigned char>(cue[0]) >= 0xE0) return true;
    for (const char* p = cue; *p != '\0'; ++p) {
        if (static_cast<unsigned char>(*p) >= 0x80) return false;
    }
    return true;
}

bool starts_with(const std::string& s, const char* cue) {
    const std::size_t n = std::strlen(cue);
    if (s.size() < n || s.compare(0, n, cue) != 0) return false;
    return !needs_boundary(cue) || s.size() == n || s[n] == ' ';
}

bool ends_with(const std::string& s, const char* cue) {
    const std::size_t n = std::strlen(cue);
    if (s.size() < n || s.compare(s.size() - n, n, cue) != 0) return false;
    return !needs_boundary(cue) || s.size() == n || s[s.size() - n - 1] == ' ';
}

// Removes `cue` and the space after it from the front of `s`.
std::string after(const std::string& s, const char* cue) {
    std::string rest = s.substr(std::strlen(cue));
    if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
    return rest;
}

// Sounds that carry no request. Longest first wherever one starts another.
const char* const kFillers[] = {
    "那个", "这个", "就是", "嗯", "呃", "额", "啊", "哦", "唔", "诶", "欸",
    "hmm", "um", "uh", "er",
};

std::string without_leading_fillers(std::string s) {
    for (bool more = true; more && !s.empty();) {
        more = false;
        for (std::size_t i = 0; i < sizeof(kFillers) / sizeof(kFillers[0]); ++i) {
            if (starts_with(s, kFillers[i])) {
                s = after(s, kFillers[i]);
                more = true;
                break;
            }
        }
    }
    return s;
}

// Opening a reply-in-progress's undoing, said with nothing after it.
const char* const kStops[] = {
    "别说了", "不用了", "不要了", "好了好了", "算了", "打住", "停下", "停", "等一下", "等等",
    "never mind", "nevermind", "stop", "wait",
};

// Opening a correction, followed by what was meant instead.
const char* const kCorrections[] = {
    "我说错了", "我的意思是", "说错了", "我是说", "应该是", "不对", "不是", "错了", "不要", "不用",
    "换成", "改成", "不", "actually", "no",
};

// How speech ends when the request in it has not come yet: an opener that
// needs what follows ("我想问一下"), a connective ("然后"), a filler.
//
// Left out because a finished sentence ends with them too: a connective of
// one character ("和" ends "暖和"); "这个" and "那个" ("我想要这个"); "就是"
// ("对，就是"); "能不能" and "可不可以" ("这样可不可以"); "对了" ("说对了"),
// which counts only alone -- see kOpenAlone.
const char* const kOpenEndings[] = {
    "问一下", "问问", "想问", "想知道", "想了解", "想说", "我想", "觉得", "我感觉", "认为",
    "在想", "帮我", "查一下", "查查", "看一下", "看看", "告诉我", "你知道", "你说", "请问",
    "你能", "我要", "我需要", "怎么说呢", "比如说", "比如",
    "然后", "所以", "但是", "可是", "而且", "因为", "如果", "还有", "就是说", "或者", "另外",
    "那么", "以及", "的话", "还是", "除了",
    "嗯", "呃", "额",
    "i was wondering", "i want to", "id like to", "i think", "can you", "could you",
    "because", "and", "but", "so", "um", "uh",
};

// Unfinished only when nothing else was said.
const char* const kOpenAlone[] = {"对了", "还有就是", "我跟你说", "跟你说"};

template <std::size_t N>
const char* first_prefix(const std::string& s, const char* const (&cues)[N]) {
    for (std::size_t i = 0; i < N; ++i) {
        if (starts_with(s, cues[i])) return cues[i];
    }
    return nullptr;
}

}  // namespace

TurnCue turn_cue(const std::string& text) {
    const std::string s = normalize(text);

    // Taking back what is being answered: a stop, or a correction. Checked
    // first, so "不对，那个" is a stop and not unfinished.
    const char* cue = first_prefix(s, kStops);
    if (cue == nullptr) cue = first_prefix(s, kCorrections);
    if (cue != nullptr) {
        return without_leading_fillers(after(s, cue)).empty() ? TurnCue::Stop : TurnCue::Correct;
    }

    const std::string words = without_leading_fillers(s);
    if (words.empty()) {
        return TurnCue::Unfinished;
    }
    for (std::size_t i = 0; i < sizeof(kOpenAlone) / sizeof(kOpenAlone[0]); ++i) {
        if (words == kOpenAlone[i]) return TurnCue::Unfinished;
    }
    // On the words before the leading fillers came off, which an ending can
    // reach into: "就是说" ends with itself, not with "说".
    for (std::size_t i = 0; i < sizeof(kOpenEndings) / sizeof(kOpenEndings[0]); ++i) {
        if (ends_with(s, kOpenEndings[i])) return TurnCue::Unfinished;
    }
    return TurnCue::Answer;
}

}  // namespace voice
}  // namespace yoai
