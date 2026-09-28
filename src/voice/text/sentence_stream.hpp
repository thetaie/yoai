// Text cut into pieces that can each be spoken on their own, including text that
// is still arriving.
//
// Split out of the reader because nothing in it is about audio. A language
// model writes its reply a token at a time, and anything that wants to act on
// whole sentences before the reply is finished -- speaking them, today; showing
// or translating them, possibly -- has the same two problems: where a cut may
// fall, and when a cut can no longer move. split_sentences() answers the first
// for a finished passage, and SentenceStream answers the second for one that is
// still being written.
//
// **The defaults are tuned for speech**, and the comments on SplitOptions say
// what each one was measured against. A caller with a different use for the
// pieces should expect to choose its own.
//
// The punctuation rules are written for this app's text: Chinese first, with
// the odd English word or number mixed in. terminates_here() in the .cpp says
// what is and is not handled.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace yoai {
namespace voice {

// One piece of the passage, as a byte range into it.
//
// `text` is the EXACT slice, punctuation and trailing whitespace included, not
// a cleaned-up version. That is what makes the invariant below checkable, and a
// caller that wants the spoken form trims it -- which is what SentenceStream
// does.
struct Sentence {
    std::string text;
    std::size_t begin = 0;   // byte offset into the passage
    std::size_t end   = 0;   // one past the last byte
};

// Where the cuts are allowed to fall.
//
// Both counts are in CHARACTERS, not bytes: a Chinese sentence of twenty
// characters is sixty bytes, and a limit expressed in bytes would cut Chinese
// three times as often as English for no reason anybody could see.
struct SplitOptions {
    // Below this, a piece is NOT cut even at a full stop -- it keeps going and
    // absorbs the next sentence.
    //
    // It exists because of a failure that is easy to reproduce and hard to
    // notice: "你好。" and "你觉得呢？" are complete sentences of three and five
    // characters, and synthesized alone they come out clipped and oddly
    // stressed, because there is not enough text for the model to build a
    // prosody arc over. Worse, an earlier cut here dropped them entirely.
    //
    // **30 rather than the 12 that only avoided that failure**, and the
    // difference was heard rather than computed. At 12 a listener's verdict on
    // the split take was that it is correct but "less natural than the whole
    // one, the pace too even"; at 30, on the same passage and seed, "better".
    // A piece holding two or three sentences gives the model room to plan a
    // pace across them, which is the one thing anchoring the voice cannot do --
    // see Reader::set_reference_voice in voice/tts/reader.hpp.
    //
    // **Its cost is on a clock the reader cannot see, and the two clocks are
    // easy to conflate.** There are two "time to first audio":
    //
    //   1. From the reader seeing the first WORD to hearing the first sound.
    //      The one a person experiences.
    //   2. From text entering the engine to the first samples leaving it. What
    //      ReaderResult::first_audio_seconds reports.
    //
    // This number barely touches (2) -- 0.15 s at 12 against 0.12 s at 30,
    // because the engine streams and the first chunk of a piece arrives in
    // about that long however long the piece is. It sets (1) almost entirely:
    // in the streaming case it is how much the language model must have written
    // before anything can be sent, and the wait is for a COMPLETE sentence of
    // at least this many characters, not for this many characters.
    //
    // So its price depends on the lane writing the text, not on the one
    // speaking it. The on-device model decodes at 14 tok/s (design/voice.md),
    // where eighteen further characters is on the order of a second; a remote
    // model is several times faster and it is a fraction of that. Neither is
    // measured by Reader::read(), which is handed text that already exists --
    // (1) only becomes measurable from an incremental entry point, where the
    // clock starts at the first token rather than at the call.
    //
    // One passage, one listener, one judgement. It is the right default until
    // somebody sits through a longer one.
    std::size_t min_chars = 30;

    // Above this a piece is cut even with no punctuation to cut at, at the last
    // comma if there is one and at a character boundary if there is not.
    //
    // A run-on with no full stop would otherwise become one enormous say(), and
    // the whole point of cutting is to have something to hand the player early.
    std::size_t max_chars = 60;

    // The LAST piece gets its own, higher minimum, and merges backwards until
    // it reaches it.
    //
    // **A short final utterance does not get a release.** The model stops
    // generating while the last vowel is still sounding, so the reply ends at
    // -34 dBFS instead of at the noise floor -- heard as abrupt, with a click
    // that a fade softens and does not remove, because what is missing is the
    // release rather than a smooth edge. Measured on one passage, cutting it
    // four ways:
    //
    //   min_chars   final piece        ending
    //   12          24 chars, 4.60 s   cut, -34.5 dBFS
    //   16          24 chars, 4.60 s   cut, -34.5
    //   20          24 chars, 4.60 s   cut, -34.5
    //   24          48 chars, 9.83 s   released, -67.8
    //
    // The same final sentence, cut three ways into the same short piece, ends
    // abruptly every time; folded into a longer one it does not. It is the
    // length of the last utterance, not the number of pieces.
    //
    // **And lengthening it is free.** Every other piece trades size against how
    // soon it can be spoken; the last one is heard when the listener is already
    // listening, so nothing waits on it. That is why this is a separate number
    // rather than a reason to raise min_chars.
    //
    // 40 sits between the two measured points and is fitted to exactly those
    // two. A passage with no piece long enough simply ends up as one.
    std::size_t tail_chars = 40;

    // Whether a comma ends a piece once min_chars has been reached, the same
    // way a full stop does.
    //
    // OFF by default, because the unit it produces is a clause and the unit
    // that sounded best was a sentence or two -- the pieces a model plans a
    // pace across. Commas are otherwise only a fallback: a piece runs to
    // max_chars and is then cut at the last one.
    //
    // **On a stream that fallback is far too late.** A reply of 45 characters
    // with its only full stop at the end has commas at 14 and 28, and with this
    // off nothing could be spoken until all 45 had been written -- measured,
    // both of its pieces were dispatched in the same millisecond, 481 ms before
    // the writer finished. With it on, the clause ending at character 28 goes
    // as soon as it arrives.
    //
    // So this is the trade the streaming case makes and the buffered one does
    // not: a shorter unit, spoken sooner.
    bool clause_cuts = false;

    // The pieces AFTER the first, which nobody is waiting on in the same way.
    //
    // The first piece is what the listener waits for, so it is cut as soon as
    // min_chars allows. Every later one is heard only once the piece before it
    // has finished playing, so it can wait for more text -- and a longer piece
    // is a better one: it gives the model room to plan a pace across sentences,
    // which is why 30 characters sounded better than 12 (see min_chars).
    //
    // These are the minimum and the maximum for those pieces, and **only a full
    // stop ends one**: clause_cuts does not apply, and a comma is used only as
    // the fallback at later_max_chars. 0 leaves min_chars, max_chars and
    // clause_cuts in force for every piece, which is how it was before.
    //
    // The wait is bounded by the listener, not by these numbers. A consumer
    // that knows how much audio is still queued calls
    // SentenceStream::set_urgent() when it is about to run out, and the next
    // piece is then cut by the first piece's rules instead -- see the reader.
    std::size_t later_min_chars = 0;
    std::size_t later_max_chars = 0;
};

// Cut `text` into pieces that can each be synthesized on their own.
//
// **The invariant is that nothing is lost**: the pieces are contiguous, in
// order, and their byte ranges cover the whole passage. Concatenating every
// `text` reproduces the input exactly. This is stated because the version of
// this that was written inline in the desktop client did not hold it -- it
// advanced a cursor past sentences it decided were too short, and they were
// silently never spoken. An invariant that can be checked is the difference.
//
// An empty passage gives an empty vector. A passage with no terminator at all
// gives one piece, or several if it is longer than `max_chars`.
std::vector<Sentence> split_sentences(const std::string& text,
                                      const SplitOptions& opt = SplitOptions());

// Codepoints in `s`, the unit SplitOptions is expressed in. A malformed byte
// counts as one.
std::size_t count_chars(const std::string& s);

// Text arriving a token at a time, handed back as pieces once they are settled.
//
// A piece is settled when no further text could change where it ends. That is
// the whole contract, because a piece handed to a consumer -- an engine that
// has started speaking it -- cannot be taken back.
//
// Every piece comes back trimmed of surrounding whitespace, and pieces that
// are nothing but whitespace are not returned. What comes back is exactly what
// the consumer should act on.
//
// Not thread-safe; one writer.
class SentenceStream {
public:
    explicit SentenceStream(const SplitOptions& opt = SplitOptions());

    // Start over with new options, dropping anything not yet settled.
    void reset(const SplitOptions& opt);

    // Append `delta` and return whatever is now settled, in order. Usually
    // empty: most tokens do not finish a sentence.
    std::vector<std::string> feed(const std::string& delta);

    // No more text. Returns every remaining piece, with the tail rule applied
    // to what was still pending.
    //
    // **The tail rule cannot reach pieces that feed() already returned**, so
    // the last piece returned here may still be shorter than tail_chars. A
    // consumer that has not started on the piece before it can merge the two
    // itself -- the reader does, when that piece is still queued.
    std::vector<std::string> finish();

    // The listener is about to run out of audio: cut the NEXT piece by the
    // first piece's rules -- min_chars, clause_cuts -- rather than waiting for
    // later_min_chars. It lasts for one piece; the caller sets it again if the
    // listener is still short. Does nothing when later_min_chars is 0.
    void set_urgent(bool on) { urgent_ = on; }
    bool urgent() const { return urgent_; }

    // Whatever is settled now, under the current rules -- for a caller whose
    // rules just changed through set_urgent() with no new text to feed.
    std::vector<std::string> poll() { return drain(false); }

    const SplitOptions& options() const { return opt_; }

private:
    std::vector<std::string> drain(bool closing);
    SplitOptions current() const;
    bool settled_at_end(const std::string& text, const SplitOptions& opt) const;

    SplitOptions opt_;
    std::string  pending_;   // text not yet settled into a piece
    std::size_t  pieces_out_ = 0;   // pieces handed back since the last reset
    bool         urgent_ = false;
};

}  // namespace voice
}  // namespace yoai
