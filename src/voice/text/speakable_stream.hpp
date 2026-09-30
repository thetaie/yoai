// A language model's reply, turned into the text a speech engine should read.
//
// The reply is markdown written for the eye, and the engine reads every
// character it is given: a reply containing `**榨菜**` was spoken with the
// asterisks in it, and a bulleted list read its bullets. This keeps the words
// and drops the markup, and it turns the line structure markdown relies on
// into the pauses speech relies on.
//
// It lives in the core rather than in each host because it was first written
// in the desktop client, where it could only see the reply in fragments, and
// every bug it had came from that: a line break at a fragment's edge trimmed
// away, so a list's "上海" and "南接" were read as one word; a bullet arriving as
// its own delta, so it was no longer at a line start and was not recognised.
// Here the text is a stream with state, so a line is a line however it was cut.
// A host passes the model's deltas through unchanged.
//
// What it does, and deliberately no more -- this is not a markdown parser:
//
//   - A line break is a full stop, so a list item, a heading or a table row
//     ends as a sentence. It is also where the sentence cutter can cut, which
//     is what lets a list be spoken item by item as it arrives. "。" is added
//     ("." after ASCII); a comma or colon ending the line is REPLACED by it --
//     "包括：" becomes "包括。" -- and a line already ending in a full stop gets
//     nothing, because two in a row are read as a hesitation, an audible "呃".
//     A blank line, or one of marks only, adds nothing.
//   - At the start of a line, the markers that open it go: list bullets, quote
//     marks, heading hashes. A line of nothing but marks -- a rule, a table's
//     separator row -- goes entirely, and so does a code fence line. The code
//     between fences stays: it is words, and dropping it silently would be
//     worse than reading it.
//   - Inline: `*` and backticks go, `**` and `__` and `~~` go, a single `_` or
//     `~` stays -- `Q4_K_M` and "3~5 度" are words. A link keeps its label and
//     loses its URL, which is the worst thing here to hear read out. A table's
//     `|` becomes a pause.
//   - Emoji and the characters that join them go. Numbers stay, including the
//     digit of a keycap emoji, so a numbered item is still read as its number.
//   - Runs of spaces become one.
//
// A lone `*` in "2*3" is dropped with the rest. Telling it from emphasis would
// mean holding text until a closing marker that may never come, and a reply is
// spoken while it is written.

#pragma once

#include <cstddef>
#include <string>

namespace yoai {
namespace voice {

// A reply arriving a token at a time. Not thread-safe; one writer.
//
// feed() returns the speakable text that is final so far. Very little is held
// back: an incomplete UTF-8 sequence, the markers at the start of a line until
// its first word shows whether they are markup, a `_`, `~`, `!`, comma or colon
// until the next character, and a link's label until its URL ends -- at most a
// line.
class SpeakableStream {
public:
    SpeakableStream();

    // Start a new reply.
    void reset();

    std::string feed(const std::string& delta);

    // No more text: whatever was held back, resolved. No pause is added at the
    // end -- there is nothing after it to pause before.
    std::string finish();

private:
    enum Link { kNone, kLabel, kAfterLabel, kUrl };

    void on_codepoint(unsigned cp, const char* bytes, std::size_t len);
    void inline_codepoint(unsigned cp, const char* bytes, std::size_t len);
    void line_end();
    void resolve_prefix();
    void abort_link();
    void flush_hold();
    void replay(const std::string& text);
    void put(unsigned cp, const char* bytes, std::size_t len);
    void put_ascii(char c);

    std::string out_;        // final text not yet returned
    std::string partial_;    // an incomplete UTF-8 sequence from the last delta
    std::string prefix_;     // marks at a line start, not yet resolved
    std::string label_;      // a link's label, raw
    std::string soft_;       // a comma or colon, until the next character shows
                             // whether a line break replaces it
    std::size_t url_bytes_;  // how much of a link's URL has been skipped
    Link        link_;
    char        hold_;       // '_', '~' or '!' waiting on the next character
    bool        line_start_;
    bool        drop_line_;  // inside a code fence line
    bool        space_owed_;
    bool        pipe_owed_;
    bool        replaying_;  // a label being put back: '[' and '!' are literal
    bool        emitted_;    // anything at all has been produced
    bool        line_text_;  // this line has produced something
    unsigned    last_;       // the last produced codepoint, closing brackets skipped
};

// A whole reply at once: feed() and finish() on a fresh stream.
std::string speakable(const std::string& text);

}  // namespace voice
}  // namespace yoai
