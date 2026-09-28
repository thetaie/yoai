#include "store/vector_index.hpp"

#include "store/sql.hpp"

#include <sqlite3.h>

#include <mutex>

namespace yoai {
namespace store {

namespace {

using sql::Stmt;
using sql::exec;
using sql::fail;

// With the connection's lock already held.
bool remove_locked(sqlite3* db, long long id, std::string* err) {
    Stmt s(db, "DELETE FROM embeddings WHERE rowid = ?;");
    if (!s.ok()) {
        return fail(db, "remove: prepare", err);
    }
    s.bind(1, id);
    if (s.step() != SQLITE_DONE) {
        return fail(db, "remove", err);
    }
    return true;
}

}  // namespace

bool VectorIndex::put(long long id, const std::vector<float>& v, std::string* err) {
    if (v.empty()) {
        if (err) {
            *err = "put: empty vector";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(db_->lock());
    sqlite3* db = db_->handle();

    // Delete then insert: vec0 has no INSERT OR REPLACE. A savepoint rather than
    // BEGIN so this nests inside a transaction the caller may already have open.
    if (!exec(db, "SAVEPOINT vector_put;", 0)) {
        return fail(db, "put: savepoint", err);
    }

    bool ok = remove_locked(db, id, err);
    if (ok) {
        Stmt s(db, "INSERT INTO embeddings(rowid, embedding) VALUES(?, ?);");
        if (!s.ok()) {
            ok = fail(db, "put: prepare", err);
        } else {
            s.bind(1, id);
            s.bind(2, v);
            if (s.step() != SQLITE_DONE) {
                ok = fail(db, "put", err);
            }
        }
    }

    // A savepoint is released either way. ROLLBACK TO alone would leave it open
    // on the stack.
    if (!ok) {
        exec(db, "ROLLBACK TO vector_put;", 0);
    }
    exec(db, "RELEASE vector_put;", 0);
    return ok;
}

bool VectorIndex::remove(long long id, std::string* err) {
    std::lock_guard<std::mutex> lock(db_->lock());
    return remove_locked(db_->handle(), id, err);
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

    std::lock_guard<std::mutex> lock(db_->lock());
    sqlite3* db = db_->handle();

    // `k = ?` rather than LIMIT: it is how vec0 learns the count up front, and
    // it keeps working when this query later joins other tables.
    Stmt s(db,
           "SELECT rowid, distance FROM embeddings"
           " WHERE embedding MATCH ? AND k = ?"
           " ORDER BY distance;");
    if (!s.ok()) {
        return fail(db, "nearest: prepare", err);
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
        return fail(db, "nearest", err);
    }
    return true;
}

}  // namespace store
}  // namespace yoai
