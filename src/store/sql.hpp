// The few SQLite helpers every file in store/ uses. Internal to store/.
//
// Statements are prepared per call rather than cached: they run at human speed
// -- a tap, a screen open, one search -- and a cache would buy nothing while
// adding a lifetime to get wrong.

#pragma once

#include <sqlite3.h>

#include <string>
#include <vector>

namespace yoai {
namespace store {
namespace sql {

// A prepared statement that finalizes itself.
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
    // As a BLOB of packed float32 in native byte order, which is what vec0
    // reads. Every target this builds for is little-endian, which is also what
    // vec0 writes to disk.
    void bind(int i, const std::vector<float>& v) {
        sqlite3_bind_blob(stmt_, i, v.data(), static_cast<int>(v.size() * sizeof(float)),
                          SQLITE_TRANSIENT);
    }
    void bind_null(int i) { sqlite3_bind_null(stmt_, i); }

    int step() { return sqlite3_step(stmt_); }

private:
    Stmt(const Stmt&);
    Stmt& operator=(const Stmt&);
    sqlite3_stmt* stmt_;
    int           rc_;
};

inline std::string col_text(sqlite3_stmt* s, int i) {
    const unsigned char* p = sqlite3_column_text(s, i);
    return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
}

// Fill `err` from the connection's last error and return false, so a failure
// path is one line.
inline bool fail(sqlite3* db, const char* what, std::string* err) {
    if (err) {
        *err = std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "no database");
    }
    return false;
}

// One or more statements with nothing to bind and nothing to read.
inline bool exec(sqlite3* db, const char* sql, std::string* err) {
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

}  // namespace sql
}  // namespace store
}  // namespace yoai
