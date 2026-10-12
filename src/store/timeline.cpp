#include "store/timeline.hpp"

#include "store/sql.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <utility>

namespace yoai {
namespace store {

namespace {

//------------------------------------------------------------------------------
// Time
//------------------------------------------------------------------------------

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::tm local_tm(std::time_t t) {
    std::tm out;
    std::memset(&out, 0, sizeof(out));
#ifdef _WIN32
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

// Read a broken-down time back as if its fields were UTC. The inverse of
// gmtime, which the standard library still does not portably provide.
std::time_t as_utc(std::tm tm) {
#ifdef _WIN32
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

// Minutes east of UTC at that instant -- derived from the two renderings of the
// same timestamp rather than from a global, so it is right across a DST edge
// and needs no tzset bookkeeping.
int offset_of(long long ms) {
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    return static_cast<int>((as_utc(local_tm(t)) - t) / 60);
}

// The same arithmetic the generated columns do, for the one case SQL cannot
// answer: what today is. Kept beside offset_of so the two cannot drift.
int day_of(long long ms, int tz_minutes) {
    return static_cast<int>((ms + static_cast<long long>(tz_minutes) * 60000) / 86400000);
}

using sql::Stmt;
using sql::col_text;
using sql::exec;
using sql::fail;

}  // namespace

//------------------------------------------------------------------------------
// Timeline
//------------------------------------------------------------------------------

struct Timeline::Impl {
    std::shared_ptr<Db> db;

    explicit Impl(std::shared_ptr<Db> d) : db(std::move(d)) {}
};

Timeline::Timeline(std::shared_ptr<Db> db) : impl_(new Impl(std::move(db))) {}

Timeline::~Timeline() {
    delete impl_;
}

int Timeline::today_local_day() {
    const long long t = now_ms();
    return day_of(t, offset_of(t));
}

std::string Timeline::day_to_iso(int local_day) {
    std::time_t t = static_cast<std::time_t>(local_day) * 86400;
    std::tm     tm;
    std::memset(&tm, 0, sizeof(tm));
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return std::string(buf);
}

int Timeline::iso_to_day(const std::string& iso) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(iso.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3) {
        return -1;
    }
    if (m < 1 || m > 12 || d < 1 || d > 31) {
        return -1;
    }
    std::tm tm;
    std::memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon  = m - 1;
    tm.tm_mday = d;
    // Read back as UTC, so no timezone is consulted: a day number is already
    // offset-applied and must not pick up the device's offset a second time.
    return static_cast<int>(as_utc(tm) / 86400);
}

namespace {

// Load one entry from a row selecting kEntryCols, in that order. The two stay
// adjacent because nothing checks that they agree.
Entry read_entry(sqlite3_stmt* s) {
    Entry e;
    e.id            = sqlite3_column_int64(s, 0);
    e.state_id      = col_text(s, 1);
    e.started_at    = sqlite3_column_int64(s, 2);
    e.started_tz    = sqlite3_column_int(s, 3);
    e.ended_at      = sqlite3_column_type(s, 4) == SQLITE_NULL ? 0 : sqlite3_column_int64(s, 4);
    e.ended_tz      = sqlite3_column_type(s, 5) == SQLITE_NULL ? 0 : sqlite3_column_int(s, 5);
    e.local_day     = sqlite3_column_int(s, 6);
    e.local_day_end = sqlite3_column_type(s, 7) == SQLITE_NULL ? 0 : sqlite3_column_int(s, 7);
    return e;
}

const char* kEntryCols =
    "id, state_id, started_at, started_tz, ended_at, ended_tz, local_day, local_day_end";

bool load_notes(sqlite3* db, Entry* e, std::string* err) {
    Stmt s(db, "SELECT id, text, at FROM notes WHERE entry_id = ? ORDER BY at, id;");
    if (!s.ok()) {
        return fail(db, "prepare notes", err);
    }
    s.bind(1, e->id);
    int rc;
    while ((rc = s.step()) == SQLITE_ROW) {
        Note n;
        n.id   = sqlite3_column_int64(s.get(), 0);
        n.text = col_text(s.get(), 1);
        n.at   = sqlite3_column_int64(s.get(), 2);
        e->notes.push_back(n);
    }
    return rc == SQLITE_DONE ? true : fail(db, "read notes", err);
}

// The open entry, if there is one. `found` distinguishes "nothing open" from a
// failure -- being idle is a normal state, not an error.
bool current_entry(sqlite3* db, Entry* out, bool* found, std::string* err) {
    *found = false;
    std::string sql = std::string("SELECT ") + kEntryCols +
                      " FROM entries WHERE ended_at IS NULL;";
    Stmt s(db, sql.c_str());
    if (!s.ok()) {
        return fail(db, "prepare current", err);
    }
    int rc = s.step();
    if (rc == SQLITE_ROW) {
        *out   = read_entry(s.get());
        *found = true;
        return true;
    }
    return rc == SQLITE_DONE ? true : fail(db, "read current", err);
}

}  // namespace

bool Timeline::switch_to(const std::string& state_id, std::string* err) {
    if (state_id.empty()) {
        if (err) *err = "switch_to: state_id is required";
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->db->lock());
    sqlite3* db = impl_->db->handle();

    if (!exec(db, "BEGIN IMMEDIATE;", err)) {
        return false;
    }

    // Re-tapping the current tile is a no-op, not a new entry. Chopping a
    // stretch in two would show up later as two rows that mean one thing.
    Entry cur;
    bool  have = false;
    if (!current_entry(db, &cur, &have, err)) {
        exec(db, "ROLLBACK;", 0);
        return false;
    }
    if (have && cur.state_id == state_id) {
        return exec(db, "COMMIT;", err);
    }

    // Rule 2: one instant serves as both the end of the old entry and the start
    // of the new one, so the two abut exactly. Reading the clock twice would
    // leave a gap of however long the write took.
    const long long at = now_ms();

    const int tz = offset_of(at);

    if (have) {
        Stmt s(db, "UPDATE entries SET ended_at = ?, ended_tz = ? WHERE ended_at IS NULL;");
        if (!s.ok()) {
            exec(db, "ROLLBACK;", 0);
            return fail(db, "prepare close", err);
        }
        s.bind(1, at);
        s.bind(2, static_cast<long long>(tz));
        if (s.step() != SQLITE_DONE) {
            exec(db, "ROLLBACK;", 0);
            return fail(db, "close open entry", err);
        }
    }
    {
        Stmt s(db,
               "INSERT INTO entries(state_id, started_at, started_tz, ended_at, ended_tz)"
               " VALUES(?, ?, ?, NULL, NULL);");
        if (!s.ok()) {
            exec(db, "ROLLBACK;", 0);
            return fail(db, "prepare open", err);
        }
        s.bind(1, state_id);
        s.bind(2, at);
        s.bind(3, static_cast<long long>(tz));
        if (s.step() != SQLITE_DONE) {
            // A foreign key failure here means the id is not a known state,
            // which is a caller mistake worth saying plainly.
            std::string why = sqlite3_errmsg(db);
            exec(db, "ROLLBACK;", 0);
            if (err) {
                *err = "open entry for '" + state_id + "': " + why;
            }
            return false;
        }
    }
    return exec(db, "COMMIT;", err);
}

bool Timeline::end_current(std::string* err) {
    return end_at(now_ms(), err);
}

bool Timeline::end_at(long long at, std::string* err) {
    std::lock_guard<std::mutex> lock(impl_->db->lock());
    sqlite3* db = impl_->db->handle();

    Entry cur;
    bool  have = false;
    if (!current_entry(db, &cur, &have, err)) {
        return false;
    }
    // Rule 3: nothing is opened in its place. Already being idle is success --
    // the user asked for no current state and there is none.
    if (!have) {
        return true;
    }

    // Both bounds are checked here rather than trusted, because both failures
    // are silent in the data. An end before the start makes a negative
    // duration, which every consumer will render as something absurd rather
    // than reject. An end in the future makes an entry that is still running
    // by the clock while being closed in the table, and the elapsed time on
    // the home screen counts backwards until the clock catches up.
    if (at < cur.started_at) {
        if (err) *err = "end_at: that is before the entry started";
        return false;
    }
    if (at > now_ms()) {
        if (err) *err = "end_at: that is in the future";
        return false;
    }

    Stmt s(db, "UPDATE entries SET ended_at = ?, ended_tz = ? WHERE ended_at IS NULL;");
    if (!s.ok()) {
        return fail(db, "prepare end", err);
    }
    s.bind(1, at);
    // The offset AT THAT INSTANT, not the one in force now. They differ across
    // a daylight-saving switch, and correcting an entry the morning after one
    // would otherwise file it an hour off.
    s.bind(2, static_cast<long long>(offset_of(at)));
    if (s.step() != SQLITE_DONE) {
        return fail(db, "end current", err);
    }
    return true;
}

bool Timeline::undo(std::string* err) {
    std::lock_guard<std::mutex> lock(impl_->db->lock());
    sqlite3* db = impl_->db->handle();

    if (!exec(db, "BEGIN IMMEDIATE;", err)) {
        return false;
    }

    Entry cur;
    bool  have = false;
    if (!current_entry(db, &cur, &have, err)) {
        exec(db, "ROLLBACK;", 0);
        return false;
    }
    if (have) {
        exec(db, "ROLLBACK;", 0);
        if (err) {
            *err = "undo: something is already open; there is nothing unambiguous to undo";
        }
        return false;
    }

    long long id = 0;
    {
        Stmt s(db,
               "SELECT id FROM entries WHERE ended_at IS NOT NULL"
               " ORDER BY ended_at DESC, id DESC LIMIT 1;");
        if (!s.ok()) {
            exec(db, "ROLLBACK;", 0);
            return fail(db, "prepare undo lookup", err);
        }
        int rc = s.step();
        if (rc == SQLITE_ROW) {
            id = sqlite3_column_int64(s.get(), 0);
        } else if (rc != SQLITE_DONE) {
            exec(db, "ROLLBACK;", 0);
            return fail(db, "undo lookup", err);
        }
    }
    if (id == 0) {
        exec(db, "ROLLBACK;", 0);
        if (err) *err = "undo: nothing has ended yet";
        return false;
    }
    {
        // Both endpoints of the end go, not just the timestamp: ended_tz is
        // only meaningful with an ended_at, and a stale one left behind would
        // make local_day_end lie on the next close.
        Stmt s(db, "UPDATE entries SET ended_at = NULL, ended_tz = NULL WHERE id = ?;");
        if (!s.ok()) {
            exec(db, "ROLLBACK;", 0);
            return fail(db, "prepare undo", err);
        }
        s.bind(1, id);
        if (s.step() != SQLITE_DONE) {
            exec(db, "ROLLBACK;", 0);
            return fail(db, "undo", err);
        }
    }
    return exec(db, "COMMIT;", err);
}

bool Timeline::note_add(const std::string& text, std::string* err) {
    if (text.empty()) {
        if (err) *err = "note_add: text is required";
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->db->lock());
    sqlite3* db = impl_->db->handle();

    Entry cur;
    bool  have = false;
    if (!current_entry(db, &cur, &have, err)) {
        return false;
    }
    if (!have) {
        if (err) *err = "note_add: no current state to attach a note to";
        return false;
    }

    Stmt s(db, "INSERT INTO notes(entry_id, text, at) VALUES(?, ?, ?);");
    if (!s.ok()) {
        return fail(db, "prepare note", err);
    }
    s.bind(1, cur.id);
    s.bind(2, text);
    s.bind(3, now_ms());
    if (s.step() != SQLITE_DONE) {
        return fail(db, "add note", err);
    }
    return true;
}

bool Timeline::now(Now* out, std::string* err) {
    std::lock_guard<std::mutex> lock(impl_->db->lock());
    sqlite3* db = impl_->db->handle();
    *out = Now();

    if (!current_entry(db, &out->current, &out->has_current, err)) {
        return false;
    }
    if (out->has_current && !load_notes(db, &out->current, err)) {
        return false;
    }

    std::string sql = std::string("SELECT ") + kEntryCols +
                      " FROM entries WHERE ended_at IS NOT NULL"
                      " ORDER BY ended_at DESC, id DESC LIMIT 1;";
    Stmt s(db, sql.c_str());
    if (!s.ok()) {
        return fail(db, "prepare last ended", err);
    }
    int rc = s.step();
    if (rc == SQLITE_ROW) {
        out->last_ended     = read_entry(s.get());
        out->has_last_ended = true;
        if (!load_notes(db, &out->last_ended, err)) {
            return false;
        }
    } else if (rc != SQLITE_DONE) {
        return fail(db, "read last ended", err);
    }
    return true;
}

bool Timeline::recent(int hours, std::vector<Entry>* out, std::string* err) {
    if (hours <= 0) {
        if (err) *err = "recent: hours must be positive";
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->db->lock());
    sqlite3* db = impl_->db->handle();
    out->clear();

    // Overlap, not containment: a stretch that began before the window and is
    // still running is the most relevant thing on the screen, and one that
    // began before it but ended inside it is part of what just happened. Only
    // an entry that finished before the cutoff drops out.
    //
    // No timezone arithmetic anywhere here -- a window is a span of real time,
    // and that is the same span wherever its owner happens to be standing.
    const long long cutoff = now_ms() - static_cast<long long>(hours) * 3600000LL;

    std::string sql = std::string("SELECT ") + kEntryCols +
                      " FROM entries WHERE ended_at IS NULL OR ended_at > ?"
                      " ORDER BY started_at DESC, id DESC;";
    Stmt s(db, sql.c_str());
    if (!s.ok()) {
        return fail(db, "prepare recent", err);
    }
    s.bind(1, cutoff);
    int rc;
    while ((rc = s.step()) == SQLITE_ROW) {
        out->push_back(read_entry(s.get()));
    }
    if (rc != SQLITE_DONE) {
        return fail(db, "read recent", err);
    }
    for (size_t i = 0; i < out->size(); ++i) {
        if (!load_notes(db, &(*out)[i], err)) {
            return false;
        }
    }
    return true;
}

bool Timeline::day(int local_day, std::vector<Entry>* out, std::string* err) {
    std::lock_guard<std::mutex> lock(impl_->db->lock());
    sqlite3* db = impl_->db->handle();
    out->clear();

    // An entry touches a day when the day falls anywhere between its two end
    // days -- not when it equals one of them. Two cases make that distinction
    // load-bearing rather than pedantic:
    //
    //   * A stretch nobody switched out of for a long weekend spans five days.
    //     Matching only the endpoints loses the three in the middle, and loses
    //     them silently: the day just looks empty.
    //   * local_day_end can be LESS than local_day. Fly west across the date
    //     line -- Beijing 02:00 to Honolulu, eleven hours -- and you land on the
    //     previous calendar date, because the offset dropped eighteen hours and
    //     the flight was shorter than that. min/max rather than a direct range
    //     is what keeps that entry findable instead of matching nothing.
    //
    // An open entry has no end day at all, so it covers every day from its
    // start onward. That is what keeps a state running since last night on
    // today's screen.
    std::string sql = std::string("SELECT ") + kEntryCols +
                      " FROM entries WHERE (ended_at IS NULL AND local_day <= ?1)"
                      " OR (?1 BETWEEN min(local_day, local_day_end)"
                      "             AND max(local_day, local_day_end))"
                      " ORDER BY started_at, id;";
    Stmt s(db, sql.c_str());
    if (!s.ok()) {
        return fail(db, "prepare day", err);
    }
    s.bind(1, static_cast<long long>(local_day));
    int rc;
    while ((rc = s.step()) == SQLITE_ROW) {
        out->push_back(read_entry(s.get()));
    }
    if (rc != SQLITE_DONE) {
        return fail(db, "read day", err);
    }
    for (size_t i = 0; i < out->size(); ++i) {
        if (!load_notes(db, &(*out)[i], err)) {
            return false;
        }
    }
    return true;
}

}  // namespace store
}  // namespace yoai
