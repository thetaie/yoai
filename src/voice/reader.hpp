// A passage read aloud: split into sentences, synthesized in parallel, emitted
// in order as it is ready.
//
// This sits ON TOP of Qtts rather than beside it. qtts.hpp speaks one utterance
// and blocks; this takes a whole reply, cuts it, runs several engines over the
// pieces at once, and hands the audio out in reading order so a player can
// start on the first sentence while the last is still being generated.
//
// **It reopens a decision this project made the other way, and the reason that
// decision was made turns out to have been a different setting.** Synthesizing
// a reply sentence by sentence was tried, judged by ear to step in timbre
// between pieces, and removed. Measured since, on one passage of four sentences
// through the 1.7B CustomVoice, comparing a whole take against a split one:
//
//   seed        pitch spread between utterances     level spread
//   pinned      14.0 Hz whole -> 16.3 split         0.0053 -> 0.0071
//   free (-1)   10.1 Hz whole -> 17.5 split         0.0043 -> 0.0092
//
// A free seed is redrawn PER say(), so a split passage draws one per sentence
// where a whole one draws a single seed for the lot, and the spreads above are
// what that costs. Pinning one removes it, which is why this class does not
// pass the engine's -1 through. See set_seed().
//
// **Those aggregates hide the failure that matters, and a listener found it in
// the first take they played.** Spread and step are computed over pause-cut
// utterances, so ONE sentence generated in the wrong register is averaged in
// with five that were fine. Measured per PIECE instead, against the median of
// the pieces -- which is the only view where a single bad one stands out:
//
//   seed   piece 0   piece 1   piece 2   piece 3
//   23     +0.00     +0.10     **-4.96** -1.27      <- heard, and reported
//   101    -0.34      0.00      -1.05    +1.72
//   202     0.00     -0.13      -0.97    +1.42
//   777    -0.18     +1.96      -0.06     0.00
//
// Semitones. **One piece in sixteen came out five semitones low** -- a whole
// sentence in a different register, not a rough joint -- and the passage it
// happened in was the first one anybody listened to. The same sentence is
// within a semitone on the three other seeds, so it is not the text: it is an
// occasional bad draw.
//
// **This is not splitting making each piece worse. It is splitting multiplying
// the exposure.** qtts.hpp already records that the engine occasionally draws a
// take with something wrong in it -- a half five decibels louder than the
// other -- and that nothing in the API prevents it. One utterance is one draw,
// and a bad one spoils a reply that is then obviously worth generating again. N
// pieces are N draws, so the chance of at least one goes up with the count, and
// the damage lands on one sentence in the middle of a reply that is otherwise
// right, which is both harder to notice in testing and worse to listen to.
//
// **Narrowing the sampler was tried and is not the answer** -- see
// Reader::set_sampling and the table in qtts.hpp. At argmax on the acoustic
// stack the excursion still happens, and on average it happens more, so this is
// not a draw that is too wide. It is a draw with nothing to hold it: every
// piece is conditioned on a speaker token and no acoustic history, so nothing
// tells piece 2 what register piece 1 was in.
//
// That is also what upstream says. A qwentts.cpp wrapper reports the same
// symptom on sentence-by-sentence cloning -- one sentence close to the
// reference, the next a different voice -- and the fix is `xvec_only`: condition
// on the speaker EMBEDDING alone rather than on embedding plus reference codes.
// qwen.h draws the same line: mode A is the reference WITHOUT its transcript,
// mode B is both. Qtts had only ever wired mode B, which matters more than it
// sounds -- the measurement that set the Base checkpoint aside, RTF 1.97
// against VoiceDesign's 1.16, was mode B, whose reference sits in context for
// every frame. Mode A holds no context and had never been measured at all.
//
// **set_reference_voice() is that mode, and it works.** Same passage, same four
// seeds, every piece anchored to one embedding from one 22.4-second recording
// of a real speaker, against the same four unanchored:
//
//   seed   spread, anchored   unanchored (custom_voice)
//   23     1.36 st            5.06 st
//   101    1.68               2.77
//   202    0.59               2.39
//   777    1.59               2.14
//
// The gross excursion is gone on all four, the typical spread drops from 3.09
// semitones to 1.31, and **the worst anchored run is tighter than the best
// unanchored one**. An earlier version of this table used a reference that was
// itself synthesized, and read 1.68 mean / 2.12 worst -- a real recording
// anchors better than a clone of one does.
//
// **And it is not the expensive mode**: measured back to back on the same
// passage, base with a mode A reference ran at RTF 0.57-0.59 against
// custom_voice's 0.57 -- with a 22.4-second reference, the length that costs
// RTF 1.97 in mode B. That is the whole difference between the two: mode B
// holds the reference in context for every frame, mode A holds a vector.
//
// What it costs instead is the written voice. `base` takes no instruct and no
// named speaker, so "warm and clear, natural pace" stops meaning anything and
// the voice becomes whichever recording is shipped as the reference. That is a
// product decision rather than a measurement, and it is the one left open here.
//
// **What anchoring does NOT fix is the pace.** With the register held, a
// listener's remaining objection to the split take was that it is less natural
// than the whole one and "the pace is too even" -- which is what independent
// pieces produce: a whole take varies its speed across the passage, and pieces
// generated separately each start from the same sentence-initial state, so
// every one of them is read at the same speed.
//
// Cutting less often is the only lever on that, and it worked: the same passage
// at min_chars 30 instead of 12 -- two pieces instead of four -- was judged
// better. SplitOptions::min_chars carries that default and the reason.
//
// It is not measurable with anything here, and that is worth saying rather than
// leaving a gap. Characters per second per piece comes out at a spread of 0.47
// for four pieces and 0.46 for two, because a per-piece average is exactly the
// statistic that cannot see a pace which RESETS at each boundary. Measuring it
// would need per-sentence alignment in the whole take, which the pause cut does
// not give -- it found eight units in four sentences.
//
// The remaining way out, not taken: re-synthesize a piece whose pitch sits too
// far from the median of its neighbours, which is computable here, costs that
// piece's latency, and cannot protect piece 0.
//
// What splitting buys was, before measuring, thought to be two things:
//
//   - **Earlier first audio on a long passage.** The first sentence is short,
//     so it is ready long before a whole paragraph would be.
//   - **More than one engine working at once**, which is the only lever on this
//     lane's throughput that does not need a different model.
//
// **Measured, neither survives on this machine.** One say() already streams, so
// it reaches the speaker in 0.13 s against this passage's 0.16 s -- the first
// claim was true of a buffered engine and this one is not buffered. The second
// is the table below. What is left, and it is the real reason to have this
// class, is the thing neither of them was about:
//
//   - **A passage that does not exist yet can be started.** The chat lane
//     writes its reply a token at a time over several seconds. A single say()
//     cannot begin until the last token has arrived; a split one can speak the
//     first sentence while the model is still writing the third. That is
//     seconds no amount of streaming recovers, because the text is what the
//     engine is waiting for.
//
// read() takes a COMPLETE passage, so it does not do that yet. This class is
// the cutting, the ordering and the handover; feeding it incrementally is a
// second entry point on top, not a rewrite.
//
// **The second one does not pay on this desktop, and the number is here so
// nobody has to find that out twice.** One passage, 19.84 s of speech, 1.7B
// CustomVoice Q4_K_M on the Arc iGPU:
//
//   voices   elapsed   first audio   starved   held
//   1        11.39 s   0.16 s        0.10 s    0.64 s
//   2        11.80 s   0.31 s        1.00 s    3.76 s
//   2        11.58 s   0.33 s        1.02 s    3.76 s   (threads halved)
//
// A second engine is slightly SLOWER, reaches the first word twice as late, and
// costs 1.4 GB. The thread count was halved to answer the obvious objection and
// changed nothing, on a machine with 22 of them.
//
// The reasoning that predicted otherwise is still worth reading, because it is
// right about the first half: qtts.hpp records 4, 6 and 8 threads landing
// within 2% of each other, which is what a stage that has stopped scaling with
// threads looks like -- and such a stage is where running two side by side
// normally wins. It does not win here, and the shape of the failure is the same
// one the omni lane's Vulkan attempt produced a day earlier: the work was
// already using the device as hard as it can be used, so splitting it into two
// queues moved it around rather than making it smaller.
//
// **What is NOT ruled out**: a CPU-only build, where the stages contend
// differently, and a phone, where they contend differently again. The
// measurement above is one machine in the configuration it deploys in.
//
// **So `voices` above 1 is not the reason to use this class today.** Splitting
// is, and only for what splitting alone gives -- see the next paragraph.
//
// **Each voice is a whole engine, so N voices is N times 1.4 GB.** They do not
// share weights: qwentts.cpp holds them per context, and nothing in its API
// offers a second session over one model. On a phone that alone settles the
// count at one, which is why `voices` is a parameter rather than a constant
// derived from the core count.
//
// Rate is 24000 Hz mono float throughout, as everywhere else on this side.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "voice/qtts.hpp"

namespace bodyclock {
namespace voice {

//------------------------------------------------------------------------------
// Splitting
//------------------------------------------------------------------------------

// One piece of the passage, as a byte range into it.
//
// `text` is the EXACT slice, punctuation and trailing whitespace included, not
// a cleaned-up version. That is what makes the invariant below checkable, and a
// caller that wants the spoken form trims it -- which is what Reader does.
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
    // see set_reference_voice.
    //
    // **Its cost is on a clock this class cannot see, and the two clocks are
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
    // So its price depends on the lane writing the text, not on this one. The
    // on-device model decodes at 14 tok/s (design/voice.md), where eighteen
    // further characters is on the order of a second; a remote model is several
    // times faster and it is a fraction of that. Neither is measured here,
    // because read() is handed text that already exists -- (1) only becomes
    // measurable from an incremental entry point, where the clock starts at the
    // first token rather than at the call.
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

//------------------------------------------------------------------------------
// Reading
//------------------------------------------------------------------------------

// Audio for the passage, in reading order.
//
// `sentence` is the index of the piece these samples belong to, so a caller can
// tell where the seams are -- for a log, or to fade across them.
//
// **`last` arrives on a call of its own, with `n_samples` 0**, once for the
// whole passage. That is deliberately the same shape as the streaming
// QttsAudioSink rather than a flag on the final block: a host that already
// handles one handles this, and the end of the last sentence is not knowable
// until the engine says so.
//
// Return false to stop. Generation is abandoned, in-flight sentences are
// cancelled, and read() returns true with what was produced -- an abort is the
// host stopping, not a failure, the same as every other lane here.
//
// **Called from a thread this class owns**, not from the caller of read() and
// not from an engine's compute worker. Unlike the Qtts sink it is allowed to be
// slow: the engines write into buffers, and only this emitter touches the host.
// A slow sink delays playback, it does not stall generation.
typedef bool (*ReaderAudioSink)(const float* pcm,
                                std::size_t n_samples,
                                int sentence,
                                bool last,
                                void* user_data);

// What one passage cost.
struct ReaderResult {
    int   sentences      = 0;
    float speech_seconds = 0.0f;

    // Wall clock for the whole passage, and the ratio the lane is judged on.
    // Compare `elapsed / speech_seconds` against the same figure from a single
    // say() over the same text: that difference IS the answer to whether
    // running several engines at once buys anything.
    double elapsed = 0.0;

    // When the first samples reached the sink, from the call to read().
    //
    // **From the CALL, which is not from the reader's first word.** The passage
    // already exists by the time read() is entered, so this is the engine's
    // latency and not a person's: what somebody waiting on a streamed reply
    // experiences also contains the time the language model spent writing
    // enough text to cut, which is the larger of the two and is set by
    // SplitOptions::min_chars. Nothing here can see it. See the note there.
    double first_audio_seconds = 0.0;

    // **Seconds the emitter spent with nothing to hand over while sentences
    // were still being generated.** The number to read alongside the ratio.
    //
    // A player consumes one second of audio per second, so this is the silence
    // it would have had to sit through if it had started at the first sample
    // and played straight on. Zero means the engines stayed ahead of the
    // listener for the whole passage; anything else is the shape of the gap,
    // not just its total.
    double starved_seconds = 0.0;

    // The most audio ever held back at once, waiting for an earlier sentence to
    // finish so it could be emitted in order.
    //
    // This is the price of ordered output with several engines running: a
    // sentence finished out of turn has to be kept. It is reported because it
    // is also the memory, at 96 KB per second held.
    float held_seconds = 0.0f;
};

// Several engines over one passage.
//
// NOT copyable and NOT re-entrant: one read() at a time per instance, the same
// contract Qtts has for say().
class Reader {
public:
    Reader();
    ~Reader();

    // Whether this build has the speech engine compiled in. False makes load()
    // fail with a message and changes nothing else -- split_sentences() is
    // plain text processing and works either way.
    static bool available();

    // Read the two GGUFs into `voices` engines. BLOCKS for all of them.
    //
    // `voices` below 1 is treated as 1. **Each one is a full copy of the
    // weights**, loaded separately, so this is `voices` times the time and the
    // memory of a single Qtts::load. One voice is a valid and useful
    // configuration: the splitting and the ordered streaming still apply, and
    // the passage is simply synthesized a sentence at a time.
    //
    // A failure part way through leaves nothing loaded.
    bool load(const std::string& talker_path, const std::string& codec_path, int voices);
    bool loaded() const;
    void unload();

    // How many engines are loaded, 0 before a load.
    int voices() const;

    // What the loaded talker is: "voice_design", "base" or "custom_voice". The
    // engines are all the same checkpoint, so this is the first one's answer.
    std::string model_type() const;

    // The named speakers a custom_voice checkpoint carries. Empty otherwise.
    std::vector<std::string> speakers() const;

    // Applied to EVERY engine, because a passage read in two voices is not a
    // passage being read. Take effect on the next read().
    void set_voice(const std::string& instruct);
    void set_speaker(const std::string& name);

    // Anchor every piece to one voice, from a recording, without its
    // transcript -- qwen.h's clone mode A. **Base checkpoint only**, and it
    // must be called after load().
    //
    // This is the candidate fix for the register excursion in the file
    // comment, and the reason it is a candidate rather than a setting: nothing
    // else here gives two independent say() calls anything in common beyond a
    // speaker token, and the excursion is what that costs. A shared embedding
    // is something in common that does not grow with the passage.
    //
    // Its price is the one that set the Base checkpoint aside in the first
    // place, and it is not the same price: the 70% that measured RTF 1.97 was
    // mode B, with the reference's codes in context for every frame. Mode A
    // holds no context. See Qtts::set_reference_voice.
    bool set_reference_voice(const std::string& wav_path);

    // The seed, and **the setting this whole class depends on**.
    //
    // A free seed (-1) is redrawn per say(), so a split passage draws one per
    // sentence where a whole one draws a single seed for all of it. On a single
    // say() that costs variety within one utterance and nothing else; across
    // sentences it is the difference between a voice and four takes of a voice.
    //
    // Measured, and it is the headline of the file comment: free, the level
    // spread between utterances more than doubles against a whole take and the
    // pitch spread rises by two thirds; pinned, neither moves. The seam that got
    // sentence-by-sentence synthesis removed from this project was four draws
    // rather than four generations.
    //
    // So this defaults to a FIXED seed rather than passing the engine's -1
    // through, and a caller that wants the engine's own behaviour has to ask
    // for it -- in the knowledge that asking is what makes the seams audible.
    void set_seed(long long seed);

    // The sampler, both stacks. See QttsSampling.
    //
    // **It is NOT the lever on the register excursion above**, which is worth
    // saying here because it is the obvious guess and it was tried. Narrowing
    // `subtalker_temperature` to argmax made the piece-to-piece spread worse on
    // average over four seeds and produced a fresh four-semitone excursion of
    // its own; qtts.hpp has the table. The excursion is the absence of acoustic
    // conditioning between pieces, not the width of the draw.
    //
    // What it is good for is everything a narrower draw does control, and a
    // split passage wants a narrower draw on those axes for the same reason it
    // wants a pinned seed: the variation is paid once per piece instead of once
    // per reply.
    //
    // Applied to every engine, and takes effect on the next read().
    void set_sampling(const QttsSampling& sampling);
    const QttsSampling& sampling() const;

    // Worker threads per engine, for the NEXT load(). <= 0 leaves upstream's
    // heuristic alone.
    //
    // **This is the setting that decides whether `voices` can pay.** Upstream
    // picks hardware_concurrency() / 2 for ONE engine; leaving that in place
    // and then running three of them asks for 1.5x the machine's threads, and
    // they spend the difference fighting each other. The count that makes a
    // comparison mean anything is one that divides the machine by `voices`.
    void set_threads(int n);

    // Where the cuts fall. Takes effect on the next read().
    void set_split(const SplitOptions& opt);
    const SplitOptions& split() const;

    // Read the whole passage. BLOCKS until the last sample has been emitted.
    //
    // `sink` NULL generates and discards, which is a measurement mode and the
    // only way to time the engines without a consumer in the way -- the same
    // meaning it has in Qtts::say.
    //
    // Returns false on a failure, with last_error() set. **The first failing
    // sentence ends the passage**: whatever is wrong -- a missing instruct, a
    // checkpoint that refuses a parameter -- the next sentence would fail the
    // same way, and a passage that emits one error per sentence is noise.
    bool read(const std::string& text, ReaderAudioSink sink, void* user_data,
              ReaderResult* out);

    //--------------------------------------------------------------------------
    // The same thing, for text that does not exist yet
    //--------------------------------------------------------------------------
    //
    // **This is what the class is for.** read() above needs the whole reply
    // before it can start, and the reply takes seconds to write -- so the wait
    // a person experiences is the language model's, not the engine's, and a
    // streamed utterance cannot recover it because the text is what it is
    // waiting for. These three let the speech start on the first sentence while
    // the model is still writing the third.
    //
    //   begin(sink, user)   start the engines and the ordered handover
    //   feed(delta)         add text; whatever is now a settled piece is
    //                       dispatched, and the last one is held back because
    //                       another token could extend it
    //   finish(out)         no more text; BLOCKS until the last sample is out
    //
    // feed() returns immediately. Cancel by returning false from the sink, as
    // with read(), and then call finish() anyway to join the threads.
    //
    // **The tail rule is weaker here**, and that is inherent rather than an
    // oversight: a short final piece can only be merged backwards while the
    // piece before it is still queued. Usually it is -- the text finishes long
    // before the speech does -- but an engine that has already started one
    // cannot be asked to take more.
    bool begin(ReaderAudioSink sink, void* user_data);
    bool feed(const std::string& delta);
    bool finish(ReaderResult* out);

    // Why the last load() or read() returned false. Empty after a success.
    const std::string& last_error() const;

    // The rate every sink receives.
    static int sample_rate();

private:
    Reader(const Reader&);
    Reader& operator=(const Reader&);

    struct Impl;
    Impl* impl_;
};

}  // namespace voice
}  // namespace bodyclock
