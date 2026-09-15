// The day log's rules, exercised.
//
// These are not "does SQLite work" tests. Each one pins a rule from
// store/day_store.hpp that is cheap to break and expensive to notice: the ones
// about gaps and about the single open entry fail silently in the data rather
// than loudly at runtime, and would surface weeks later as a day that does not
// add up.
//
// Run it directly -- it takes a scratch directory and leaves nothing behind:
//
//     build/bodyclock-store-test <dir>

#include "store/day_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using bodyclock::store::DayStore;
using bodyclock::store::Entry;
using bodyclock::store::Now;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

// Every call that should succeed goes through here, so a rule failing for an
// unrelated reason still prints why.
bool did(bool ok, const std::string& err, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s: %s\n", what, err.c_str());
        ++g_failures;
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bodyclock-store-test <scratch-dir>\n");
        return 2;
    }
    std::string err;
    DayStore*   s = DayStore::open(argv[1], &err);
    if (!s) {
        std::fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }

    // --- a fresh store is idle, and that is not an error ----------------------
    Now n;
    did(s->now(&n, &err), err, "now() on an empty store");
    check(!n.has_current, "a fresh store has no current state");
    check(!n.has_last_ended, "a fresh store has nothing to undo");
    check(!s->undo(&err), "undo with nothing ended is refused");
    check(!s->note_add("orphan", &err), "a note with no current state is refused");

    // --- switching opens one entry -------------------------------------------
    did(s->switch_to("work", &err), err, "switch to work");
    did(s->now(&n, &err), err, "now() after switching");
    check(n.has_current && n.current.state_id == "work", "work is current");
    check(n.current.open(), "the current entry is open");
    check(!n.current.local_date.empty(), "the entry carries a local date");

    // --- rule 2: a switch leaves no gap and no overlap ------------------------
    const long long first_started = n.current.started_at;
    did(s->switch_to("eat", &err), err, "switch to eat");
    did(s->now(&n, &err), err, "now() after the second switch");
    check(n.has_current && n.current.state_id == "eat", "eat is current");
    check(n.has_last_ended && n.last_ended.state_id == "work", "work is what just ended");
    check(n.last_ended.ended_at == n.current.started_at,
          "the old entry ends exactly where the new one starts");
    check(n.last_ended.started_at == first_started, "the old entry kept its start");

    // --- re-tapping the current state does not chop the entry -----------------
    const long long eat_started = n.current.started_at;
    did(s->switch_to("eat", &err), err, "switch to eat again");
    did(s->now(&n, &err), err, "now() after re-tapping");
    check(n.current.started_at == eat_started, "re-tapping the current tile changes nothing");
    check(n.last_ended.state_id == "work", "and does not end anything");

    // --- notes attach to the open entry --------------------------------------
    did(s->note_add("noodles", &err), err, "add a note");
    did(s->note_add("too salty", &err), err, "add a second note");
    did(s->now(&n, &err), err, "now() after notes");
    check(n.current.notes.size() == 2, "both notes are on the current entry");
    check(n.current.notes[0].text == "noodles", "notes come back oldest first");

    // --- rule 3 and 4: ending opens nothing, and the gap stays a gap ----------
    did(s->end_current(&err), err, "end the current state");
    did(s->now(&n, &err), err, "now() after ending");
    check(!n.has_current, "ending leaves no current state");
    check(n.has_last_ended && n.last_ended.state_id == "eat", "eat is what just ended");
    check(n.last_ended.ended_at != 0, "the ended entry has an end");

    std::vector<Entry> today;
    did(s->day(DayStore::today_local_date(), &today, &err), err, "read today");
    check(today.size() == 2, "the gap is not a row -- two states, not three");

    did(s->end_current(&err), err, "ending again while idle is a no-op");
    did(s->day(DayStore::today_local_date(), &today, &err), err, "read today again");
    check(today.size() == 2, "a second end did not add anything");

    // --- undo reopens the last entry and absorbs the gap ----------------------
    did(s->undo(&err), err, "undo");
    did(s->now(&n, &err), err, "now() after undo");
    check(n.has_current && n.current.state_id == "eat", "undo put eat back");
    check(n.current.open(), "the reopened entry is open again");
    check(n.current.notes.size() == 2, "undo did not lose the notes");
    check(!s->undo(&err), "undo is refused while something is open");

    // --- rule 1, from the other side -----------------------------------------
    did(s->day(DayStore::today_local_date(), &today, &err), err, "read today after undo");
    int open_rows = 0;
    for (size_t i = 0; i < today.size(); ++i) {
        if (today[i].open()) {
            ++open_rows;
        }
    }
    check(open_rows == 1, "exactly one entry is open");

    // --- an unknown state is refused rather than stored -----------------------
    check(!s->switch_to("not-a-state", &err), "an unknown state id is refused");
    check(!s->switch_to("", &err), "an empty state id is refused");
    did(s->now(&n, &err), err, "now() after the refused switches");
    check(n.has_current && n.current.state_id == "eat", "the refusals changed nothing");

    // --- a day nobody lived in is empty, not an error -------------------------
    std::vector<Entry> none;
    did(s->day("1970-01-01", &none, &err), err, "read a day with no entries");
    check(none.empty(), "an unlived day comes back empty");

    delete s;
    std::printf("\n%s\n", g_failures ? "FAILED" : "all rules hold");
    return g_failures ? 1 : 0;
}
