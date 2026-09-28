// The timeline's rules, exercised.
//
// These are not "does SQLite work" tests. Each one pins a rule from
// store/timeline.hpp that is cheap to break and expensive to notice: the ones
// about gaps and about the single open entry fail silently in the data rather
// than loudly at runtime, and would surface weeks later as a day that does not
// add up.
//
// Run it directly -- it takes a scratch directory and leaves nothing behind:
//
//     build/yoai-timeline-test <dir>

#include "store/timeline.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using yoai::store::Db;
using yoai::store::Timeline;
using yoai::store::Entry;
using yoai::store::Now;

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

long long test_now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Write one entry straight into the file, at times the test picks.
//
// The store reads the real clock, so neither a stretch across midnight nor a
// state that has been open for hours can be produced through its API inside a
// test that runs in a second. This is a fixture, not a second way to write: it
// exists to exercise the queries and the corrections, which are the parts that
// were wrong.
//
// An `ended_at` of 0 seeds an entry that is still open. Only one such row can
// exist at a time -- that is the point of the partial unique index -- so the
// caller has to have closed whatever was open first.
//
// Through the store's own connection, under its lock: a second connection to
// the file is what store/db.hpp exists to prevent, fixtures included.
bool seed_entry(Db& store, const char* state,
                long long started_at, int started_tz,
                long long ended_at, int ended_tz) {
    std::lock_guard<std::mutex> lock(store.lock());
    sqlite3* db = store.handle();
    sqlite3_stmt* st = 0;
    sqlite3_prepare_v2(db,
        "INSERT INTO entries(state_id, started_at, started_tz, ended_at, ended_tz)"
        " VALUES(?, ?, ?, ?, ?);", -1, &st, 0);
    sqlite3_bind_text(st, 1, state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, started_at);
    sqlite3_bind_int(st, 3, started_tz);
    if (ended_at == 0) {
        sqlite3_bind_null(st, 4);
        sqlite3_bind_null(st, 5);
    } else {
        sqlite3_bind_int64(st, 4, ended_at);
        sqlite3_bind_int(st, 5, ended_tz);
    }
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) {
        std::printf("FAIL  fixture: %s\n", sqlite3_errmsg(db));
        ++g_failures;
    }
    sqlite3_finalize(st);
    return ok;
}

// Local wall-clock time on a given day number, as a UTC timestamp.
long long at_local(int day, int hour, int minute, int tz_minutes) {
    return static_cast<long long>(day) * 86400000LL + hour * 3600000LL +
           minute * 60000LL - static_cast<long long>(tz_minutes) * 60000LL;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: yoai-timeline-test <scratch-dir>\n");
        return 2;
    }
    std::string         err;
    std::shared_ptr<Db> db = Db::open(Db::path_in(argv[1]), &err);
    if (!db) {
        std::fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }
    Timeline* s = new Timeline(db);

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
    check(n.current.local_day == Timeline::today_local_day(), "the entry lands on today");
    check(n.current.local_day_end == 0, "an open entry has no end day yet");

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
    did(s->day(Timeline::today_local_day(), &today, &err), err, "read today");
    check(today.size() == 2, "the gap is not a row -- two states, not three");

    did(s->end_current(&err), err, "ending again while idle is a no-op");
    did(s->day(Timeline::today_local_day(), &today, &err), err, "read today again");
    check(today.size() == 2, "a second end did not add anything");

    // --- undo reopens the last entry and absorbs the gap ----------------------
    did(s->undo(&err), err, "undo");
    did(s->now(&n, &err), err, "now() after undo");
    check(n.has_current && n.current.state_id == "eat", "undo put eat back");
    check(n.current.open(), "the reopened entry is open again");
    check(n.current.notes.size() == 2, "undo did not lose the notes");
    check(!s->undo(&err), "undo is refused while something is open");

    // --- rule 1, from the other side -----------------------------------------
    did(s->day(Timeline::today_local_day(), &today, &err), err, "read today after undo");
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
    did(s->day(0, &none, &err), err, "read a day with no entries");
    check(none.empty(), "an unlived day comes back empty");

    // --- a still-open entry covers every day from its start onward ------------
    const int today_num = Timeline::today_local_day();
    std::vector<Entry> tomorrow;
    did(s->day(today_num + 1, &tomorrow, &err), err, "read tomorrow");
    check(tomorrow.size() == 1 && tomorrow[0].open(),
          "the open entry is still covering tomorrow");

    // --- day numbers survive a round trip through a date ----------------------
    check(Timeline::iso_to_day(Timeline::day_to_iso(today_num)) == today_num,
          "day -> date -> day round-trips");
    check(Timeline::day_to_iso(0) == "1970-01-01", "day 0 is the epoch");
    check(Timeline::iso_to_day("not a date") == -1, "an unparseable date is refused");

    // --- an entry that crosses midnight belongs to BOTH days ------------------
    //
    // Last night's sleep: 23:30 to 07:00, one stretch, one row.
    const int kTz = 480;   // +08:00 at both ends -- nobody moved
    if (seed_entry(*db, "nap",
                   at_local(today_num - 1, 23, 30, kTz), kTz,
                   at_local(today_num, 7, 0, kTz), kTz)) {
        std::vector<Entry> last_night, this_morning;
        did(s->day(today_num - 1, &last_night, &err), err, "read yesterday");
        did(s->day(today_num, &this_morning, &err), err, "read today with the sleep in it");

        int found_yesterday = 0, found_today = 0;
        for (size_t i = 0; i < last_night.size(); ++i) {
            if (last_night[i].state_id == "nap") ++found_yesterday;
        }
        for (size_t i = 0; i < this_morning.size(); ++i) {
            if (this_morning[i].state_id == "nap") ++found_today;
        }
        check(found_yesterday == 1, "the overnight entry is on the day it started");
        check(found_today == 1, "and on the day it ended -- the morning is not a gap");
        check(last_night.size() > 0 && this_morning.size() > 0,
              "both days came back non-empty");
    }

    // --- and so does one that crosses a timezone on the way -------------------
    //
    // Leaves at 17:00 in San Francisco, lands at 15:00 in Beijing the next day.
    // The two endpoints are fourteen hours and two offsets apart.
    if (seed_entry(*db, "commute",
                   at_local(today_num - 3, 17, 0, -420), -420,
                   at_local(today_num - 2, 15, 0, 480), 480)) {
        std::vector<Entry> dep, arr;
        did(s->day(today_num - 3, &dep, &err), err, "read the departure day");
        did(s->day(today_num - 2, &arr, &err), err, "read the arrival day");
        int on_dep = 0, on_arr = 0;
        for (size_t i = 0; i < dep.size(); ++i) {
            if (dep[i].state_id == "commute") ++on_dep;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            if (arr[i].state_id == "commute") ++on_arr;
        }
        check(on_dep == 1, "the flight is on the day it left");
        check(on_arr == 1, "and on the day it landed, in the other timezone");
        if (on_dep == 1) {
            for (size_t i = 0; i < dep.size(); ++i) {
                if (dep[i].state_id != "commute") continue;
                check(dep[i].started_tz == -420 && dep[i].ended_tz == 480,
                      "each endpoint kept its own offset");
            }
        }
    }

    // --- a stretch nobody switched out of covers the days in between ----------
    if (seed_entry(*db, "rest",
                   at_local(today_num - 30, 10, 0, kTz), kTz,
                   at_local(today_num - 26, 10, 0, kTz), kTz)) {
        for (int d = today_num - 30; d <= today_num - 26; ++d) {
            std::vector<Entry> got;
            int found = 0;
            did(s->day(d, &got, &err), err, "read a day inside the long stretch");
            for (size_t i = 0; i < got.size(); ++i) {
                if (got[i].state_id == "rest") ++found;
            }
            check(found == 1, d == today_num - 30   ? "long stretch: first day"
                            : d == today_num - 26   ? "long stretch: last day"
                                                    : "long stretch: a day in the middle");
        }
    }

    // --- flying west over the date line ends a stretch on an EARLIER day ------
    //
    // Beijing 02:00 to Honolulu, eleven hours: the offset drops eighteen, so
    // the landing is on the previous calendar date. The entry's end day is less
    // than its start day, and it still has to be findable on both.
    if (seed_entry(*db, "commute",
                   at_local(today_num - 10, 2, 0, 480), 480,
                   at_local(today_num - 11, 19, 0, -600), -600)) {
        std::vector<Entry> dep, arr;
        did(s->day(today_num - 10, &dep, &err), err, "read the day it left");
        did(s->day(today_num - 11, &arr, &err), err, "read the earlier day it landed on");
        int on_dep = 0, on_arr = 0;
        for (size_t i = 0; i < dep.size(); ++i) {
            if (dep[i].state_id == "commute" && dep[i].started_tz == 480) ++on_dep;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            if (arr[i].state_id == "commute" && arr[i].started_tz == 480) ++on_arr;
        }
        check(on_dep == 1, "date line: found on the departure day");
        check(on_arr == 1, "date line: found on the arrival day, which is earlier");
        for (size_t i = 0; i < dep.size(); ++i) {
            if (dep[i].state_id == "commute" && dep[i].started_tz == 480) {
                check(dep[i].local_day_end < dep[i].local_day,
                      "date line: the end day really is before the start day");
            }
        }
    }

    // --- ending in the past is how a forgotten state gets corrected -----------
    //
    // Stopped working three hours ago, noticed now. A real clock cannot be
    // rewound inside a test that runs in a second, so the open entry is seeded
    // with a start in the past.
    did(s->end_current(&err), err, "close whatever was open first");
    const long long began = test_now_ms() - 3 * 3600000LL;
    if (seed_entry(*db, "work", began, kTz, 0, 0)) {
        check(!s->end_at(began - 1000, &err), "an end before the start is refused");
        check(!s->end_at(test_now_ms() + 3600000LL, &err), "an end in the future is refused");
        did(s->now(&n, &err), err, "now() after the refused ends");
        check(n.has_current, "a refused end left the entry open");

        const long long stopped = began + 2 * 3600000LL;   // it really stopped at two hours
        did(s->end_at(stopped, &err), err, "end as of a past instant");
        did(s->now(&n, &err), err, "now() after backdating");
        check(!n.has_current, "the entry is closed");

        // Found by its start rather than through last_ended: this fixture put an
        // open entry alongside one that was already closed, which overlaps and
        // is something the API itself cannot produce. last_ended orders by when
        // entries ENDED, so it answers about the other one here.
        const Entry* closed = 0;
        std::vector<Entry> win;
        did(s->recent(24, &win, &err), err, "find the backdated entry");
        for (size_t i = 0; i < win.size(); ++i) {
            if (win[i].started_at == began) {
                closed = &win[i];
            }
        }
        check(closed != 0, "the backdated entry is still there");
        if (closed) {
            check(closed->ended_at == stopped, "it ended when we said, not when we asked");
            check(closed->ended_at - closed->started_at == 2 * 3600000LL,
                  "so it lasted the two hours that happened, not the three on the clock");
        }

        did(s->end_at(stopped, &err), err, "ending again while idle is still a no-op");

        // Leave something open for the window checks below, through the real
        // API rather than the fixture.
        did(s->switch_to("work", &err), err, "open something for the checks that follow");
    }

    // --- the rolling window does not stop at midnight -------------------------
    //
    // The fixtures above put a sleep across last midnight, a flight ten days
    // back and a five-day stretch a month back. A 24-hour window should hold
    // the first and the still-open entry, and neither of the old ones.
    {
        std::vector<Entry> win;
        did(s->recent(24, &win, &err), err, "read the last 24 hours");
        int napped = 0, open_now = 0, ancient = 0;
        for (size_t i = 0; i < win.size(); ++i) {
            if (win[i].state_id == "nap") ++napped;
            if (win[i].open()) ++open_now;
            if (win[i].local_day < today_num - 2) ++ancient;
        }
        check(napped == 1, "last night's sleep is in the last 24 hours");
        check(open_now == 1, "so is whatever is still running");
        check(ancient == 0, "and nothing from last week is");
        check(win.size() >= 2 && win[0].started_at >= win[win.size() - 1].started_at,
              "the window comes back newest first");

        std::vector<Entry> wide;
        did(s->recent(24 * 400, &wide, &err), err, "read a very wide window");
        check(wide.size() > win.size(), "a wider window holds more");
        check(!s->recent(0, &win, &err), "a window of no hours is refused");
    }

    delete s;
    std::printf("\n%s\n", g_failures ? "FAILED" : "all rules hold");
    return g_failures ? 1 : 0;
}
