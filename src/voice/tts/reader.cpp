#include "voice/tts/reader.hpp"

#include "utils/log.hpp"
#include "voice/text/speakable_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace yoai {
namespace voice {

namespace {

typedef std::chrono::steady_clock clk;

double seconds_since(clk::time_point start) {
    return std::chrono::duration_cast<std::chrono::duration<double> >(clk::now() - start).count();
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
// run of quiet in case more speech follows it, and keeps 80 ms of speech back so
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
//
// 80 ms, not the 20 it was. CustomVoice ends most short utterances with the
// last vowel still at about -45 dBFS -- 16 takes in 20, the same with the whole
// text up front, and a longer eos hold barely moved it -- and a 20 ms fade of
// that was still heard as the voice being cut off. 80 and 150 ms raised-cosine
// fades were listened to against it, and 80 was enough. Holding it back delays
// what goes out by 60 ms more, which nobody hears while the voice is talking.
const double kEdgeSeconds = 0.080;

// Silence after the very end of a passage, so a player that stops on the last
// sample does not stop on the fade. Only at the end: a join never gets
// synthesized silence, see the note above the class.
const double kEndSilence = 0.200;

// How little queued audio counts as about to run out, for the pacer.
//
// When every piece handed to the engines is finished and the listener has less
// than this left to hear, the next piece is cut at once instead of waiting to
// grow -- see SplitOptions::later_min_chars. It has to cover a piece's own
// start: the engine's first audio arrives 0.15-0.23 s after it is asked
// (measured per piece on the desktop GPU), plus the pacer's own polling and the
// shaper's hold. A second covers that about four times over.
const double kQueueMargin = 1.0;

// How often the pacer looks. Small against the margin above.
const int kPacerMillis = 50;

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

// The gain of a raised-cosine ramp from 1 to 0 at step i of n. It starts flat,
// so it is heard as the voice trailing off rather than as the level being
// turned down.
double fade_gain(std::size_t i, std::size_t n) {
    const double pi = 3.14159265358979323846;
    return 0.5 * (1.0 + std::cos(pi * static_cast<double>(i + 1) / static_cast<double>(n)));
}

// To zero over the whole buffer.
void fade_out(std::vector<float>* v) {
    const std::size_t n = v->size();
    for (std::size_t i = 0; i < n; ++i) {
        (*v)[i] = static_cast<float>((*v)[i] * fade_gain(i, n));
    }
}

// One piece's worth of edge state. Reset between pieces.
class EdgeShaper {
public:
    explicit EdgeShaper(int rate)
        : frame_(static_cast<std::size_t>(kFrameSeconds * rate)),
          edge_(static_cast<std::size_t>(kEdgeSeconds * rate)),
          join_(static_cast<std::size_t>(kJoinPause * rate)),
          end_silence_(static_cast<std::size_t>(kEndSilence * rate)),
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
            // released this is 80 ms of room tone faded, which is nothing; on
            // one cut mid-vowel it is the difference between a gate and a fade.
            fade_tail(out, edge_);
            out->insert(out->end(), end_silence_, 0.0f);
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
            (*out)[total - n + i] = static_cast<float>((*out)[total - n + i] * fade_gain(i, n));
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
    std::size_t end_silence_;
    bool        head_done_;
    std::size_t lead_budget_;   // leading quiet the next join still owes
    std::vector<float> carry_;   // samples not yet a whole frame
    std::vector<float> keep_;    // speech held back for a possible fade
    std::vector<float> quiet_;   // trailing run of silence, fate undecided
};

}  // namespace

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
    bool               streaming_text;
    int                eos_hold;

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
    // The whole-reply mode (Reader::set_whole_reply): the spoken text as it
    // is fed, how much of it the one generation has pulled, and whether
    // finish() has said there is no more. Guarded by `mu`.
    bool                    whole;
    std::string             pull_text;
    std::size_t             pull_taken;
    bool                    pull_closed;
    std::size_t             held;        // samples buffered and not yet emitted
    std::size_t             held_peak;
    std::size_t             emitted;     // samples handed to the sink

    // The listener's clock, kept by the emitter: the instant, in seconds from
    // begin(), at which everything handed over so far will have finished
    // playing. Guarded by `mu`; `playing` is false until the first audio.
    double                  playhead;
    bool                    playing;

    // The threads, and what the stream needs to outlive a call.
    std::vector<std::thread> pool;
    std::thread              emitter;
    std::thread              pacer;
    ReaderAudioSink          sink;
    void*                    sink_user;
    bool                     running;
    // The text side: `speakable` and `stream`, touched by feed() on the
    // writer's thread and by the pacer on its own. Always taken BEFORE `mu`,
    // never while holding it, and held across the dispatch so the pieces are
    // queued in the order they were cut.
    std::mutex               text_mu;
    // The reply as written, turned into what is spoken. Ahead of the cutter,
    // so the cuts fall on the text the engine will actually be given.
    SpeakableStream          speakable;
    SentenceStream           stream;     // text not yet settled into a piece
    clk::time_point          started;
    double                   first_audio;
    double                   starved;

    Impl() : seed(23), threads(0), streaming_text(false), eos_hold(0), next_job(0), next_emit(0),
             aborted(false), failed(false), closed(false),
             whole(false), pull_taken(0), pull_closed(false),
             held(0), held_peak(0), emitted(0),
             playhead(0.0), playing(false),
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
            engines[k]->set_streaming_text(streaming_text);
            engines[k]->set_eos_hold(eos_hold);
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

    // The whole-reply mode's text source, on the engine's compute worker:
    // blocks until feed() has added something the engine has not had, or
    // finish() has closed the text. A stopped read stops the engine too.
    static int pull_source(char* buf, int cap, void* user_data) {
        Impl* m = static_cast<Impl*>(user_data);
        std::unique_lock<std::mutex> lock(m->mu);
        m->cv.wait(lock, [m] {
            return m->aborted || m->failed || m->pull_taken < m->pull_text.size() || m->pull_closed;
        });
        if (m->aborted || m->failed) {
            return -1;
        }
        const std::size_t left = m->pull_text.size() - m->pull_taken;
        if (left == 0) {
            return 0;   // closed, and all of it handed over
        }
        const std::size_t n = std::min(left, static_cast<std::size_t>(cap));
        std::memcpy(buf, m->pull_text.data() + m->pull_taken, n);
        m->pull_taken += n;
        return static_cast<int>(n);
    }

    // The whole-reply mode's one worker: the reply as a single generation on
    // the first engine, into piece 0, which the emitter drains as it grows.
    void run_whole(bool want_audio) {
        ChunkCtx ctx;
        ctx.impl = this;
        ctx.job  = 0;
        QttsResult r;
        const bool ok = engines[0]->say_pulled(std::string(), &Impl::pull_source, this,
                                               want_audio ? &Impl::on_chunk : 0,
                                               want_audio ? &ctx : 0, &r);
        std::lock_guard<std::mutex> lock(mu);
        pieces[0].done = true;
        // A reply with nothing speakable in it -- a code block, say -- ends
        // the text before any arrives, which the engine refuses. That is an
        // empty reading, not a failure.
        const bool nothing = pull_closed && pull_text.empty();
        if (!ok && !aborted && !nothing) {
            failed = true;
            if (error.empty()) {
                error = engines[0]->last_error();
            }
        }
        if (!want_audio) {
            emitted += static_cast<std::size_t>(r.speech_seconds * Qtts::sample_rate());
        }
        cv.notify_all();
    }

    // Whether the piece just finished was the last one there will ever be.
    // On a stream that needs `closed` as well as the count: a piece that is
    // currently last is not the final one if more text is still coming, and
    // ending the passage on it would fade a join. Only the emitter moves
    // next_emit, and only it calls this.
    bool next_emit_is_past_end() {
        std::lock_guard<std::mutex> lock(mu);
        return closed && next_emit >= pieces.size();
    }

    // Queue pieces the stream has settled, so the workers can claim them.
    // `closing` is set for the last batch, which is where the tail rule can
    // still reach back into a piece that is queued but not started.
    void dispatch(const std::vector<std::string>& ready, bool closing);

    // run_emitter on a thread of its own, keeping its two results where
    // finish() can read them after the join.
    void stream_emitter() {
        starved = run_emitter(sink, sink_user, &first_audio, started);
    }

    // Whether the listener is about to run out: every piece handed to the
    // engines is finished, and what is queued -- played out by the playhead,
    // or generated and not yet emitted -- is under kQueueMargin. With a piece
    // still being generated it is not: that piece streams faster than it
    // plays. Called with `mu` held.
    bool running_dry() const {
        if (!playing || next_job < pieces.size() ||
            (!pieces.empty() && !pieces.back().done)) {
            return false;
        }
        double queued = playhead - seconds_since(started);
        if (queued < 0.0) {
            queued = 0.0;
        }
        queued += static_cast<double>(held) / Qtts::sample_rate();
        return queued < kQueueMargin;
    }

    // Lets a piece after the first wait for more text, and stops it waiting
    // once the listener is about to run out. See SplitOptions::later_min_chars.
    //
    // A thread of its own because the moment to cut is a TIME, not an event:
    // a slow writer can leave a finished sentence sitting in the stream while
    // the audio before it drains, and nothing else would look at it again
    // until the next token.
    void run_pacer() {
        for (;;) {
            bool dry;
            {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait_for(lock, std::chrono::milliseconds(kPacerMillis));
                if (aborted || failed || closed) {
                    return;
                }
                dry = running_dry();
            }
            std::lock_guard<std::mutex> text(text_mu);
            if (dry != stream.urgent()) {
                stream.set_urgent(dry);
                if (dry) {
                    dispatch(stream.poll(), false);
                }
            }
        }
    }

    // Drains the pieces in order on the emitter thread. Returns the silence a
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
                {
                    // For the pacer, which decides from it when to stop waiting
                    // for a longer piece.
                    std::lock_guard<std::mutex> lock(mu);
                    this->playhead = playhead;
                    this->playing  = true;
                }
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
            utils::log_error("reader: voice %d of %d failed to load -- %s",
                                k + 1, voices, m->error.c_str());
            return false;
        }
        m->engines.push_back(engine);
    }

    m->apply_settings();
    utils::log_info("reader: %d voice(s) loaded in %.1f s, %s",
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

int Reader::talker_hidden_size() const {
    return impl_->engines.empty() ? 0 : impl_->engines[0]->talker_hidden_size();
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

bool Reader::set_reference_embedding(const std::vector<float>& embedding) {
    Impl* m = impl_;
    m->error.clear();
    if (m->engines.empty()) {
        m->error = "set_reference_embedding needs a loaded model";
        return false;
    }
    for (std::size_t k = 0; k < m->engines.size(); ++k) {
        if (!m->engines[k]->set_reference_embedding(embedding)) {
            m->error = m->engines[k]->last_error();
            return false;
        }
    }
    return true;
}

bool Reader::load_reference_embedding(const std::string& spk_path) {
    Impl* m = impl_;
    m->error.clear();
    if (m->engines.empty()) {
        m->error = "load_reference_embedding needs a loaded model";
        return false;
    }
    // Read once, by the first engine, and handed to the rest as a vector.
    if (!m->engines[0]->load_reference_embedding(spk_path)) {
        m->error = m->engines[0]->last_error();
        return false;
    }
    for (std::size_t k = 1; k < m->engines.size(); ++k) {
        if (!m->engines[k]->set_reference_embedding(m->engines[0]->reference_embedding())) {
            m->error = m->engines[k]->last_error();
            return false;
        }
    }
    return true;
}

const std::vector<float>& Reader::reference_embedding() const {
    static const std::vector<float> kNone;
    return impl_->engines.empty() ? kNone : impl_->engines[0]->reference_embedding();
}

bool Reader::save_reference_embedding(const std::string& spk_path) {
    Impl* m = impl_;
    m->error.clear();
    if (m->engines.empty()) {
        m->error = "no reference embedding to save -- nothing is loaded";
        return false;
    }
    if (!m->engines[0]->save_reference_embedding(spk_path)) {
        m->error = m->engines[0]->last_error();
        return false;
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

void Reader::set_streaming_text(bool on) {
    impl_->streaming_text = on;
    for (std::size_t k = 0; k < impl_->engines.size(); ++k) {
        impl_->engines[k]->set_streaming_text(on);
    }
}

void Reader::set_eos_hold(int frames) {
    impl_->eos_hold = frames;
    for (std::size_t k = 0; k < impl_->engines.size(); ++k) {
        impl_->engines[k]->set_eos_hold(frames);
    }
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

void Reader::set_whole_reply(bool on) {
    impl_->whole = on;
}

const std::string& Reader::last_error() const {
    return impl_->error;
}

void Reader::Impl::dispatch(const std::vector<std::string>& ready, bool closing) {
    if (ready.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu);
    for (std::size_t k = 0; k < ready.size(); ++k) {
        const std::string& spoken = ready[k];
        // The tail rule cannot reach backwards past a piece an engine already
        // has. It still can while the last one is only queued, which is the
        // common case at the end of a reply: the text finishes faster than the
        // speech does. See SplitOptions::tail_chars.
        const bool short_tail = closing && k + 1 == ready.size() &&
                                count_chars(spoken) < stream.options().tail_chars;
        if (short_tail && !pieces.empty() && next_job < pieces.size()) {
            // Both sides were trimmed, so the whitespace that separated them
            // in the reply is gone. Chinese needs none, but two ASCII ends
            // run together -- "end.Next" -- and the engine reads that as one
            // word.
            std::string& prev = pieces.back().text;
            if (!prev.empty() && static_cast<unsigned char>(prev[prev.size() - 1]) < 0x80 &&
                static_cast<unsigned char>(spoken[0]) < 0x80) {
                prev += ' ';
            }
            prev += spoken;
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
        // with the text. utils/log.cpp now writes a record through
        // WriteConsoleW when stderr is a console, so the code page is out of
        // the path and `chcp 65001` is no longer needed to read this line.
        utils::log_info("reader: piece %d, %d char(s): %s",
                           static_cast<int>(pieces.size()) - 1,
                           static_cast<int>(count_chars(spoken)), spoken.c_str());
    }
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
    m->speakable.reset();
    m->stream.reset(m->split);
    m->playhead    = 0.0;
    m->playing     = false;
    m->next_job    = 0;
    m->next_emit   = 0;
    m->aborted     = false;
    m->failed      = false;
    m->closed      = false;
    m->pull_text.clear();
    m->pull_taken  = 0;
    m->pull_closed = false;
    m->held        = 0;
    m->held_peak   = 0;
    m->emitted     = 0;
    m->first_audio = 0.0;
    m->starved     = 0.0;
    m->sink        = sink;
    m->sink_user   = user_data;
    m->started     = clk::now();
    m->running     = true;

    const bool want_audio = (sink != 0);
    if (m->whole) {
        // One piece for the whole reply, generated by one worker as the text
        // comes. next_job past it, so nothing else claims it.
        m->pieces.push_back(Impl::Piece());
        m->next_job = 1;
        m->pool.push_back(std::thread(&Impl::run_whole, m, want_audio));
    } else {
        // Every engine gets a worker, not one per piece: on a stream the piece
        // count is not known yet, and a worker with nothing to do waits.
        m->pool.reserve(m->engines.size());
        for (std::size_t k = 0; k < m->engines.size(); ++k) {
            m->pool.push_back(std::thread(&Impl::run_worker, m, k, want_audio));
        }
    }
    if (want_audio) {
        // On its own thread, because begin() returns and there is no caller
        // left to run it on.
        m->emitter = std::thread(&Impl::stream_emitter, m);
        // Only with audio, and only when later pieces are allowed to wait:
        // without a listener there is no playhead to pace against. Nothing is
        // cut in the whole-reply mode, so there is nothing to pace.
        if (!m->whole && m->split.later_min_chars > 0) {
            m->pacer = std::thread(&Impl::run_pacer, m);
        }
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
    std::lock_guard<std::mutex> text(m->text_mu);
    if (m->whole) {
        // Cleaned, not cut: the one generation takes the text as it comes.
        const std::string spoken = m->speakable.feed(delta);
        if (!spoken.empty()) {
            std::lock_guard<std::mutex> lock(m->mu);
            m->pull_text += spoken;
            m->cv.notify_all();
        }
        return true;
    }
    m->dispatch(m->stream.feed(m->speakable.feed(delta)), false);
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

    if (m->whole) {
        // Whatever the cleaner still held, and the end of the text.
        std::lock_guard<std::mutex> text(m->text_mu);
        const std::string tail = m->speakable.finish();
        std::lock_guard<std::mutex> lock(m->mu);
        m->pull_text += tail;
        m->pull_closed = true;
        m->cv.notify_all();
        // The exact text the engine was given; see the note in dispatch().
        utils::log_info("reader: whole reply, %d char(s): %s",
                        static_cast<int>(count_chars(m->pull_text)), m->pull_text.c_str());
    } else {
        // Whatever the cleaner still held, then the cutter's remainder, as one
        // closing batch so the tail rule sees the true last piece.
        std::lock_guard<std::mutex> text(m->text_mu);
        std::vector<std::string> ready = m->stream.feed(m->speakable.finish());
        const std::vector<std::string> rest = m->stream.finish();
        ready.insert(ready.end(), rest.begin(), rest.end());
        m->dispatch(ready, true);
    }
    {
        std::lock_guard<std::mutex> lock(m->mu);
        m->closed = true;
        m->cv.notify_all();
    }
    // Nothing is left for it to cut.
    if (m->pacer.joinable()) {
        m->pacer.join();
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
        utils::log_error("reader: stopped after %.2f s -- %s", elapsed, m->error.c_str());
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
    utils::log_info("reader: %d piece(s) on %d voice(s) -- %.2f s of speech in %.2f s "
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
}  // namespace yoai
