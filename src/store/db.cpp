#include "store/db.hpp"

#include "utils/log.hpp"
#include "store/migrations.hpp"   // generated from sql/sqlite/*.sql
#include "store/sql.hpp"

#include <sqlite-vec.h>
#include <sqlite3.h>

#include <cstdio>

namespace yoai {
namespace store {

namespace {

// The applied schema version, or 0 for a database that has never been touched.
//
// Reads user_version rather than a row in a meta table: meta would itself be
// created by the first migration, so asking it what version we are at would
// require the schema it is supposed to tell us about. user_version is a header
// field that exists before any table does.
int user_version(sqlite3* db) {
    sql::Stmt s(db, "PRAGMA user_version;");
    if (!s.ok() || s.step() != SQLITE_ROW) {
        return 0;
    }
    return sqlite3_column_int(s.get(), 0);
}

// Apply every migration newer than the current version, each in its own
// transaction so a failure leaves the database at the last version that fully
// succeeded rather than half way through one.
bool migrate(sqlite3* db, std::string* err) {
    const int from = user_version(db);
    for (int i = 0; i < kMigrationCount; ++i) {
        const Migration& m = kMigrations[i];
        if (m.version <= from) {
            continue;
        }
        if (!sql::exec(db, "BEGIN IMMEDIATE;", err)) {
            return false;
        }
        if (!sql::exec(db, m.sql, err)) {
            sql::exec(db, "ROLLBACK;", 0);
            if (err) {
                *err = std::string("migration ") + m.name + ": " + *err;
            }
            return false;
        }
        // PRAGMA takes no parameters, so this one is composed. The value is an
        // int from a build-time table, never anything a user typed.
        char pragma[64];
        std::snprintf(pragma, sizeof(pragma), "PRAGMA user_version = %d;", m.version);
        if (!sql::exec(db, pragma, err) || !sql::exec(db, "COMMIT;", err)) {
            sql::exec(db, "ROLLBACK;", 0);
            return false;
        }
        utils::log_info("applied %s", m.name);
    }
    return true;
}

// vec0 and the vec_* SQL functions, on this connection only. sqlite-vec is
// compiled in, not loaded (see third/sqlite-vec/README.md), so this is a direct
// call rather than an extension load -- and per connection rather than through
// sqlite3_auto_extension, so nothing gets it without asking.
bool enable_vectors(sqlite3* db, std::string* err) {
    char* msg = 0;
    if (sqlite3_vec_init(db, &msg, 0) != SQLITE_OK) {
        if (err) {
            *err = std::string("sqlite-vec: ") + (msg ? msg : sqlite3_errmsg(db));
        }
        sqlite3_free(msg);
        return false;
    }
    return true;
}

}  // namespace

std::string Db::path_in(const std::string& dir) {
    std::string path = dir;
    if (!path.empty() && path[path.size() - 1] != '/' && path[path.size() - 1] != '\\') {
        path += "/";
    }
    return path + "yoai.db";
}

std::shared_ptr<Db> Db::open(const std::string& path, std::string* err) {
    sqlite3* raw = 0;
    if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) {
        if (err) {
            *err = "open " + path + ": " + (raw ? sqlite3_errmsg(raw) : "out of memory");
        }
        sqlite3_close(raw);
        return std::shared_ptr<Db>();
    }
    // Owned from here, so every failure below closes it.
    std::shared_ptr<Db> self(new Db(raw, path));

    // WAL so a read never blocks the write that a tap just made, and a bounded
    // busy timeout so a contended write reports itself instead of spinning.
    // synchronous=NORMAL is the WAL-appropriate setting: a power loss can cost
    // the last transactions, which here is one tap, and never the database.
    if (!sql::exec(raw, "PRAGMA journal_mode=WAL;", err) ||
        !sql::exec(raw, "PRAGMA synchronous=NORMAL;", err) ||
        !sql::exec(raw, "PRAGMA busy_timeout=3000;", err)) {
        return std::shared_ptr<Db>();
    }

    // Before the migrations, so one of them can create a vec0 table.
    if (!enable_vectors(raw, err) || !migrate(raw, err)) {
        return std::shared_ptr<Db>();
    }

    utils::log_info("database opened: %s (schema v%d)", path.c_str(), user_version(raw));
    return self;
}

Db::~Db() {
    sqlite3_close(db_);
}

int Db::schema_version() const {
    return user_version(db_);
}

}  // namespace store
}  // namespace yoai
