#include "memory/vector_index.hpp"

#include <sqlite-vec.h>
#include <sqlite3.h>

namespace yoai {
namespace memory {

namespace {

// A prepared statement that finalizes itself. The same shape as the one in
// store/timeline.cpp; the two are not shared yet because the connection is not.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : stmt_(0) {
        rc_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, 0);
    }
    ~Stmt() { sqlite3_finalize(stmt_); }

    bool ok() const { return rc_ == SQLITE_OK && stmt_ != 0; }
    sqlite3_stmt* get() { return stmt_; }

    void bind(int i, long long v) { sqlite3_bind_int64(stmt_, i, v); }
    // vec0 reads a BLOB as packed float32 in native byte order. Every target
    // this builds for is little-endian, which is also what vec0 writes to disk.
    void bind(int i, const std::vector<float>& v) {
        sqlite3_bind_blob(stmt_, i, v.data(), static_cast<int>(v.size() * sizeof(float)),
                          SQLITE_TRANSIENT);
    }

    int step() { return sqlite3_step(stmt_); }

private:
    Stmt(const Stmt&);
    Stmt& operator=(const Stmt&);
    sqlite3_stmt* stmt_;
    int           rc_;
};

bool fail(sqlite3* db, const char* what, std::string* err) {
    if (err) {
        *err = std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "no database");
    }
    return false;
}

bool exec(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, 0, 0, 0) == SQLITE_OK;
}

}  // namespace

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

bool VectorIndex::put(long long id, const std::vector<float>& v, std::string* err) {
    if (v.empty()) {
        if (err) {
            *err = "put: empty vector";
        }
        return false;
    }

    // Delete then insert: vec0 has no INSERT OR REPLACE. A savepoint rather than
    // BEGIN so this nests inside a transaction the caller may already have open.
    if (!exec(db_, "SAVEPOINT vector_put;")) {
        return fail(db_, "put: savepoint", err);
    }

    bool ok = remove(id, err);
    if (ok) {
        Stmt s(db_, "INSERT INTO embeddings(rowid, embedding) VALUES(?, ?);");
        if (!s.ok()) {
            ok = fail(db_, "put: prepare", err);
        } else {
            s.bind(1, id);
            s.bind(2, v);
            if (s.step() != SQLITE_DONE) {
                ok = fail(db_, "put", err);
            }
        }
    }

    // A savepoint is released either way. ROLLBACK TO alone would leave it open
    // on the stack.
    if (!ok) {
        exec(db_, "ROLLBACK TO vector_put;");
    }
    exec(db_, "RELEASE vector_put;");
    return ok;
}

bool VectorIndex::remove(long long id, std::string* err) {
    Stmt s(db_, "DELETE FROM embeddings WHERE rowid = ?;");
    if (!s.ok()) {
        return fail(db_, "remove: prepare", err);
    }
    s.bind(1, id);
    if (s.step() != SQLITE_DONE) {
        return fail(db_, "remove", err);
    }
    return true;
}

bool VectorIndex::nearest(const std::vector<float>& q, int k,
                          std::vector<Hit>* out, std::string* err) {
    out->clear();
    if (q.empty() || k < 1) {
        if (err) {
            *err = "nearest: needs a non-empty query and k >= 1";
        }
        return false;
    }

    // `k = ?` rather than LIMIT: it is how vec0 learns the count up front, and
    // it keeps working when this query later joins other tables.
    Stmt s(db_,
           "SELECT rowid, distance FROM embeddings"
           " WHERE embedding MATCH ? AND k = ?"
           " ORDER BY distance;");
    if (!s.ok()) {
        return fail(db_, "nearest: prepare", err);
    }
    s.bind(1, q);
    s.bind(2, static_cast<long long>(k));

    int rc;
    while ((rc = s.step()) == SQLITE_ROW) {
        Hit h;
        h.id       = sqlite3_column_int64(s.get(), 0);
        h.distance = sqlite3_column_double(s.get(), 1);
        out->push_back(h);
    }
    if (rc != SQLITE_DONE) {
        out->clear();
        return fail(db_, "nearest", err);
    }
    return true;
}

}  // namespace memory
}  // namespace yoai
