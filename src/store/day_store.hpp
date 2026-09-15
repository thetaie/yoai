// The day log: a sequence of states, each with a start and an end.
//
// This is the first thing the core persists. Everything above it -- keys,
// provider settings, which model is chosen -- still belongs to the host, and
// deliberately so: those are platform secrets and platform preferences. The day
// log is neither. It is the product's own data, the same shape on every
// platform, governed by rules that are easy to get subtly wrong, so it lives
// here once instead of being reimplemented per host.
//
// The rules, which are the reason this is not just a table:
//
//   1. At most one entry is open (ended_at unset) at any time. Enforced by a
//      partial unique index, not by care.
//   2. Switching closes the open entry and opens the new one at the SAME
//      instant, so a run of switches is contiguous with no gap and no overlap.
//   3. Ending closes the open entry and opens nothing. The time that follows is
//      a gap.
//   4. A gap is the ABSENCE of entries, never a row. Nothing fills it in later
//      on its own.
//   5. Every entry carries the local date and UTC offset in force when it was
//      written, so "today" cannot change meaning after a flight.
//
// Times are milliseconds since the Unix epoch, UTC. Zero means "unset", which
// is only ever true of Entry::ended_at.

#pragma once

#include <string>
#include <vector>

namespace bodyclock {
namespace store {

// One note attached to an entry -- the design's "add a note" on the current
// state card.
struct Note {
    long long   id   = 0;
    std::string text;
    long long   at   = 0;
};

// One stretch of time in one state.
struct Entry {
    long long         id         = 0;
    std::string       state_id;        // 'working', 'eating', ... never displayed
    long long         started_at = 0;
    long long         ended_at   = 0;  // 0 while this entry is the open one
    std::string       local_date;      // 'YYYY-MM-DD', as of started_at
    int               tz_offset  = 0;  // minutes east of UTC, as of started_at
    std::vector<Note> notes;

    bool open() const { return ended_at == 0; }
};

// What the screens need in one shot: what is happening now, and -- when nothing
// is -- what just stopped, so the idle screen can offer to undo it.
struct Now {
    bool  has_current = false;
    Entry current;

    bool  has_last_ended = false;
    Entry last_ended;
};

// One SQLite database, one process. Every method is safe to call from any
// thread.
//
// Errors are returned, never thrown: this sits directly under a C ABI, and an
// exception crossing that boundary is undefined. A false return always fills
// `err` with something a log line can carry.
class DayStore {
public:
    // Opens (creating if needed) `dir`/daylog.db and brings the schema up to
    // date. `dir` must already exist -- it is the host's sandbox directory, and
    // creating it is the host's business.
    //
    // Returns null on failure. The caller owns the result.
    static DayStore* open(const std::string& dir, std::string* err);

    ~DayStore();

    // Make `state_id` the current state as of now.
    //
    // Closes whatever was open at the same instant, so no gap appears between
    // them. Switching to the state that is ALREADY open does nothing and
    // succeeds: re-tapping the current tile should not chop the entry in two.
    bool switch_to(const std::string& state_id, std::string* err);

    // Close the open entry and open nothing, starting a gap. Succeeds and does
    // nothing when already idle.
    bool end_current(std::string* err);

    // Reopen the entry that was closed most recently, as if it had never been
    // ended.
    //
    // Only available while idle -- once something else is open there is no
    // unambiguous thing to undo. The gap that had accumulated is absorbed into
    // the reopened entry, which is the point: the screen offering this shows
    // how long the gap is, so the choice is made with the cost visible.
    bool undo(std::string* err);

    // Attach a note to the open entry. Fails while idle: a note belongs to
    // something, and the design only offers it from the current state card.
    bool note_add(const std::string& text, std::string* err);

    // The current entry and the last closed one. Both may be absent -- that is
    // a fresh install, not an error.
    bool now(Now* out, std::string* err);

    // Every entry whose LOCAL DATE is `local_date` ('YYYY-MM-DD'), oldest
    // first, each with its notes.
    //
    // Selects on the stored local date rather than on a time range, so a day is
    // the day it was lived in even if the device has since moved timezone. An
    // entry that runs past midnight belongs to the day it STARTED in; splitting
    // it for a 00-24 bar is the reader's job, which is why the UTC offset comes
    // along.
    bool day(const std::string& local_date, std::vector<Entry>* out, std::string* err);

    // The local date and UTC offset right now, by the device's clock. The
    // screens need this to ask for "today" without duplicating the conversion.
    static std::string today_local_date();

private:
    DayStore();
    DayStore(const DayStore&);
    DayStore& operator=(const DayStore&);

    struct Impl;
    Impl* impl_;
};

}  // namespace store
}  // namespace bodyclock
