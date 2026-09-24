// The timeline: a sequence of states, each with a start and an end.
//
// This is the first thing the core persists. Everything above it -- keys,
// provider settings, which model is chosen -- still belongs to the host, and
// deliberately so: those are platform secrets and platform preferences. The
// timeline is neither. It is the product's own data, the same shape on every
// platform, governed by rules that are easy to get subtly wrong, so it lives
// here once instead of being reimplemented per host.
//
// It owns three tables -- states, entries, notes -- but NOT the file they are
// in. bodyclock.db is the app's one database, and what comes next goes in it
// too: embeddings carry a foreign key into entries, and so will photos, tags
// and the rest. They are unprefixed because the file is the namespace and the
// names are already distinct; a module prefix would claim `entries` for this
// class when in fact everything else hangs off it.
//
// When a second module does arrive it must NOT open its own connection to the
// same file. Two SQLite connections in one process, each with its own locks on
// one database, is the failure that makes linking the platform's SQLite from
// native code a data-corruption bug on Android. The connection gets lifted into
// something both share, and this class becomes one facade over it. That is not
// worth building for a single occupant, so it has not been.
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
//   5. Each endpoint carries the UTC offset in force AT THAT ENDPOINT, so
//      "today" cannot change meaning after a flight and neither endpoint is
//      misplaced when the two differ.
//
// Times are milliseconds since the Unix epoch, UTC. Zero means "unset", which
// is only ever true of Entry::ended_at.
//
// A local day is a DAY NUMBER -- days since the epoch -- not a date string.
// Ranges and "seven days ago" are then arithmetic, which is what the month view
// and the insight lines are made of. Convert at the edges with day_to_iso and
// iso_to_day; nothing above this layer should see the number.
//
//------------------------------------------------------------------------------
// A day is the day it was LIVED, not the day it would be in your current
// timezone
//------------------------------------------------------------------------------
//
// This comes up again every time someone new reads the schema, usually as "why
// store the offset, wouldn't it be more flexible to project everything into
// whatever timezone the user is in right now?" It would be more flexible and it
// would be wrong.
//
// Work 09:00 to 17:00 in Beijing, then fly to London and look at that day:
//
//   as lived (+08:00)       2026-09-15   09:00 - 17:00
//   re-projected (+01:00)   2026-09-15   02:00 - 10:00
//
// Nobody was at their desk at two in the morning. Re-projecting does not shift
// one entry, it shifts EVERY past day the moment its owner changes timezone,
// and a log whose job is to say what a day looked like would then disagree with
// the only person who remembers.
//
// The one thing that does follow the current clock is which day is TODAY,
// because today is the one being lived: today_local_day() reads the offset in
// force right now, and nothing else does.
//
// Note that nothing is lost by choosing this. started_at and started_tz are the
// stored truth; local_day is derived from them, so any other projection is
// still one expression away --
//
//   SELECT date((started_at + :viewer_offset * 60000) / 86400000 * 86400,
//               'unixepoch') FROM entries
//
// -- it just is not what the day screens ask for. The derived columns exist to
// give the common question an index and a single definition, not to close a
// door.
//
// What they do pin down is MIDNIGHT as the boundary between days. Someone who
// works nights might reasonably want 04:00, so that a two-in-the-morning entry
// counts toward the evening before. That would be a schema change -- but a
// cheap one, since the derived columns are VIRTUAL and store nothing: the
// rebuild copies only the base columns and the rest recomputes.

#pragma once

#include <string>
#include <vector>

namespace yoai {
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
    std::string       state_id;        // 'work', 'eat', ... never displayed
    long long         started_at = 0;
    int               started_tz = 0;  // minutes east of UTC, at started_at
    long long         ended_at   = 0;  // 0 while this entry is the open one
    int               ended_tz   = 0;  // meaningless while open
    int               local_day     = 0;   // derived; days since the epoch
    int               local_day_end = 0;   // 0 while open
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
class Timeline {
public:
    // Opens (creating if needed) `dir`/bodyclock.db and brings the schema up to
    // date. `dir` must already exist -- it is the host's sandbox directory, and
    // creating it is the host's business.
    //
    // Returns null on failure. The caller owns the result.
    static Timeline* open(const std::string& dir, std::string* err);

    ~Timeline();

    // Make `state_id` the current state as of now.
    //
    // Closes whatever was open at the same instant, so no gap appears between
    // them. Switching to the state that is ALREADY open does nothing and
    // succeeds: re-tapping the current tile should not chop the entry in two.
    bool switch_to(const std::string& state_id, std::string* err);

    // Close the open entry and open nothing, starting a gap. Succeeds and does
    // nothing when already idle.
    bool end_current(std::string* err);

    // The same, but as of an instant that has already passed.
    //
    // This is the way out of a state nobody switched away from. Stop working at
    // 17:00, forget, and notice at 23:00: ending "now" would credit six hours
    // that did not happen, and there is no other correction -- an entry left
    // open keeps accruing forever, and the app cannot tell "still going" from
    // "forgot". So the end time is the caller's to give.
    //
    // It must not be before the entry started and must not be in the future;
    // both are refused rather than clamped, since either one means the caller
    // is working from something other than what is in the table. The time from
    // `at` to now becomes a gap, which is exactly right: nobody was in that
    // state for it.
    //
    // The UTC offset recorded is the one in force AT `at`, not the one in force
    // now -- the two differ across a daylight-saving switch.
    bool end_at(long long at, std::string* err);

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

    // Everything still going on, or finished within the last `hours` hours --
    // newest first, each with its notes.
    //
    // A ROLLING WINDOW, deliberately not a day, because the home screen asks a
    // different question than the day screens do. "What have I been doing" has
    // no reason to stop at midnight: open the app at 00:30 and a day-shaped
    // answer is one entry long while the evening it belongs to has just been
    // cut away. A window does not care where the date changed.
    //
    // Days are still the unit everywhere that COMPARES -- the day bar, the
    // month calendar, and every line of the form "on days when you napped
    // longer than 25 minutes". Two rolling windows overlap and cannot be
    // compared to each other, so this is an addition to day(), never a
    // replacement for it.
    //
    // `hours` is the caller's decision, not the store's: the store has no
    // opinion about how much counts as recent.
    bool recent(int hours, std::vector<Entry>* out, std::string* err);

    // Every entry that TOUCHES `local_day`, oldest first, each with its notes.
    //
    // Touches, not starts in: a night's sleep starts before midnight and ends
    // after, and a day view that only asked for entries starting that day would
    // draw the whole morning as a gap. A stretch nobody switched out of for
    // days covers every day in between, not just its two ends. An entry still
    // open covers every day from its start onward.
    //
    // **Do not assume local_day <= local_day_end.** Flying west across the date
    // line lands you on an earlier calendar date than you left, so an entry's
    // end day can precede its start day. Any code that walks the days an entry
    // covers has to order the two first; a loop from start to end runs zero
    // times on those entries and drops them without a word.
    //
    // Selecting on the derived day rather than on a UTC range is what keeps a
    // past day fixed: it was computed from the offset in force when it was
    // lived, so moving timezone later cannot renumber it.
    //
    // Splitting an entry at midnight for a 00-24 bar is still the reader's job.
    // That is why both endpoints carry their own offset.
    bool day(int local_day, std::vector<Entry>* out, std::string* err);

    // Today, by the device's clock right now.
    static int today_local_day();

    // Day number <-> 'YYYY-MM-DD', for the edges that speak to people. Both are
    // pure calendar arithmetic and neither consults the device's timezone: the
    // offset has already been applied by the time a day number exists.
    static std::string day_to_iso(int local_day);
    static int         iso_to_day(const std::string& iso);   // -1 if unparseable

private:
    Timeline();
    Timeline(const Timeline&);
    Timeline& operator=(const Timeline&);

    struct Impl;
    Impl* impl_;
};

}  // namespace store
}  // namespace yoai
