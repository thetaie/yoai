#include "store/day_store.hpp"

#include "platform/log.hpp"
#include "store/migrations.hpp"   // generated from sql/sqlite/*.sql

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace bodyclock {
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

std::string date_of(long long ms) {
    std::tm tm = local_tm(static_cast<std::time_t>(ms / 1000));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return std::string(buf);
}

// Minutes east of UTC at that instant -- derived from the two renderings of the
// same timestamp rather than from a global, so it is right across a DST edge
// and needs no tzset bookkeeping.
int offset_of(long long ms) {
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    return static_cast<int>((as_utc(local_tm(t)) - t) / 60);
}

//------------------------------------------------------------------------------
// SQLite helpers
//------------------------------------------------------------------------------

// A prepared statement that finalizes itself. Every query below is prepared per
// call rather than cached: these run at human speed -- a tap, a screen open --
// and a cache would buy nothing while adding a lifetime to get wrong.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : stmt_(0) {
        rc_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, 0);
    }
    ~Stmt() { sqlite3_finalize(stmt_); }

    bool ok() const { return rc_ == SQLITE_OK && stmt_ != 0; }
    sqlite3_stmt* get() { return stmt_; }

    void bind(int i, long long v) { sqlite3_bind_int64(stmt_, i, v); }
    void bind(int i, const std::string& v) {
        sqlite3_bind_text(stmt_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    }
    void bind_null(int i) { sqlite3_bind_null(stmt_, i); }

    int step() { return sqlite3_step(stmt_); }

private:
    Stmt(const Stmt&);
    Stmt& operator=(const Stmt&);
    sqlite3_stmt* stmt_;
    int           rc_;
};

std::string col_text(sqlite3_stmt* s, int i) {
    const unsigned char* p = sqlite3_column_text(s, i);
    return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
}

bool fail(sqlite3* db, const char* what, std::string* err) {
    if (err) {
        *err = std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "no database");
    }
    return false;
}

bool exec(sqlite3* db, const char* sql, std::string* err) {
    char* msg = 0;
    if (sqlite3_exec(db, sql, 0, 0, &msg) != SQLITE_OK) {
        if (err) {
            *err = std::string("exec: ") + (msg ? msg : "unknown");
        }
        sqlite3_free(msg);
        return false;
    }
    return true;
}

//------------------------------------------------------------------------------
// Schema
//------------------------------------------------------------------------------

// The applied schema version, or 0 for a database that has never been touched.
//
// Reads user_version rather than a row in meta: meta is itself created by the
// first migration, so asking it what version we are at would require the schema
// it is supposed to tell us about. user_version is a header field that exists
// before any table does.
int schema_version(sqlite3* db) {
    Stmt s(db, "PRAGMA user_version;");
    if (!s.ok() || s.step() != SQLITE_ROW) {
        return 0;
    }
    return sqlite3_column_int(s.get(), 0);
}

// Apply every migration newer than the current version, each in its own
// transaction so a failure leaves the database at the last version that fully
// succeeded rather than half way through one.
bool migrate(sqlite3* db, std::string* err) {
    const int from = schema_version(db);
    for (int i = 0; i < kMigrationCount; ++i) {
        const Migration& m = kMigrations[i];
        if (m.version <= from) {
            continue;
        }
        if (!exec(db, "BEGIN IMMEDIATE;", err)) {
            return false;
        }
        if (!exec(db, m.sql, err)) {
            exec(db, "ROLLBACK;", 0);
            if (err) {
                *err = std::string("migration ") + m.name + ": " + *err;
            }
            return false;
        }
        // PRAGMA takes no parameters, so this one is composed. The value is an
        // int from a build-time table, never anything a user typed.
        char pragma[64];
        std::snprintf(pragma, sizeof(pragma), "PRAGMA user_version = %d;", m.version);
        if (!exec(db, pragma, err) || !exec(db, "COMMIT;", err)) {
            exec(db, "ROLLBACK;", 0);
            return false;
        }
        platform::log_info("applied %s", m.name);
    }
    return true;
}

}  // namespace

//------------------------------------------------------------------------------
// DayStore
//------------------------------------------------------------------------------

struct DayStore::Impl {
    sqlite3*   db;
    std::mutex mutex;   // guards the multi-statement transactions below

    Impl() : db(0) {}
};

DayStore::DayStore() : impl_(new Impl) {}

DayStore::~DayStore() {
    if (impl_->db) {
        sqlite3_close(impl_->db);
    }
    delete impl_;
}

std::string DayStore::today_local_date() {
    return date_of(now_ms());
}

DayStore* DayStore::open(const std::string& dir, std::string* err) {
    DayStore* self = new DayStore();
    std::string path = dir;
    if (!path.empty() && path[path.size() - 1] != '/' && path[path.size() - 1] != '\\') {
        path += "/";
    }
    path += "daylog.db";

    if (sqlite3_open(path.c_str(), &self->impl_->db) != SQLITE_OK) {
        if (err) {
            *err = "open " + path + ": " +
                   (self->impl_->db ? sqlite3_errmsg(self->impl_->db) : "out of memory");
        }
        delete self;
        return 0;
    }

    sqlite3* db = self->impl_->db;

    // WAL so a read never blocks the write that a tap just made, and a bounded
    // busy timeout so a contended write reports itself instead of spinning.
    // synchronous=NORMAL is the WAL-appropriate setting: a power loss can cost
    // the last transactions, which here is one tap, and never the database.
    if (!exec(db, "PRAGMA journal_mode=WAL;", err) ||
        !exec(db, "PRAGMA synchronous=NORMAL;", err) ||
        !exec(db, "PRAGMA busy_timeout=3000;", err)) {
        delete self;
        return 0;
    }

    if (!migrate(db, err)) {
        delete self;
        return 0;
    }

    platform::log_info("day log opened: %s (schema v%d)", path.c_str(), schema_version(db));
    return self;
}

namespace {

// Load one entry from a row selecting the six columns in schema order.
Entry read_entry(sqlite3_stmt* s) {
    Entry e;
    e.id         = sqlite3_column_int64(s, 0);
    e.state_id   = col_text(s, 1);
    e.started_at = sqlite3_column_int64(s, 2);
    e.ended_at   = sqlite3_column_type(s, 3) == SQLITE_NULL ? 0 : sqlite3_column_int64(s, 3);
    e.local_date = col_text(s, 4);
    e.tz_offset  = sqlite3_column_int(s, 5);
    return e;
}

const char* kEntryCols = "id, state_id, started_at, ended_at, local_date, tz_offset";

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

bool DayStore::switch_to(const std::string& state_id, std::string* err) {
    if (state_id.empty()) {
        if (err) *err = "switch_to: state_id is required";
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    sqlite3* db = impl_->db;

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

    if (have) {
        Stmt s(db, "UPDATE entries SET ended_at = ? WHERE ended_at IS NULL;");
        if (!s.ok()) {
            exec(db, "ROLLBACK;", 0);
            return fail(db, "prepare close", err);
        }
        s.bind(1, at);
        if (s.step() != SQLITE_DONE) {
            exec(db, "ROLLBACK;", 0);
            return fail(db, "close open entry", err);
        }
    }
    {
        Stmt s(db,
               "INSERT INTO entries(state_id, started_at, ended_at, local_date, tz_offset)"
               " VALUES(?, ?, NULL, ?, ?);");
        if (!s.ok()) {
            exec(db, "ROLLBACK;", 0);
            return fail(db, "prepare open", err);
        }
        s.bind(1, state_id);
        s.bind(2, at);
        s.bind(3, date_of(at));
        s.bind(4, static_cast<long long>(offset_of(at)));
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

bool DayStore::end_current(std::string* err) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    sqlite3* db = impl_->db;

    Stmt s(db, "UPDATE entries SET ended_at = ? WHERE ended_at IS NULL;");
    if (!s.ok()) {
        return fail(db, "prepare end", err);
    }
    s.bind(1, now_ms());
    if (s.step() != SQLITE_DONE) {
        return fail(db, "end current", err);
    }
    // Rule 3: nothing is opened in its place. Already being idle is success --
    // the user asked for no current state and there is none.
    return true;
}

bool DayStore::undo(std::string* err) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    sqlite3* db = impl_->db;

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
        Stmt s(db, "UPDATE entries SET ended_at = NULL WHERE id = ?;");
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

bool DayStore::note_add(const std::string& text, std::string* err) {
    if (text.empty()) {
        if (err) *err = "note_add: text is required";
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    sqlite3* db = impl_->db;

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

bool DayStore::now(Now* out, std::string* err) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    sqlite3* db = impl_->db;
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

bool DayStore::day(const std::string& local_date, std::vector<Entry>* out, std::string* err) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    sqlite3* db = impl_->db;
    out->clear();

    std::string sql = std::string("SELECT ") + kEntryCols +
                      " FROM entries WHERE local_date = ? ORDER BY started_at, id;";
    Stmt s(db, sql.c_str());
    if (!s.ok()) {
        return fail(db, "prepare day", err);
    }
    s.bind(1, local_date);
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
}  // namespace bodyclock
