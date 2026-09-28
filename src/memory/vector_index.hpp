// Nearest-neighbor search over stored embeddings, on sqlite-vec.
//
// The vectors live in the app's one database as a vec0 virtual table named
// `embeddings`, with one float column named `embedding`:
//
//   CREATE VIRTUAL TABLE embeddings USING vec0(embedding float[N]);
//
// That statement belongs in a migration under sql/sqlite/, not here, and there
// is none yet: N is the embedding model's output size, and no model has been
// chosen. Until then only the test creates the table.
//
// This class does NOT open a connection. store/timeline.hpp says why a second
// connection to the same file is the bug to avoid; the connection is borrowed,
// and whoever owns it must have called enable_vectors() on it first.
//
// A row is keyed by rowid, which is the id of whatever the vector was computed
// from. A virtual table cannot carry a foreign key, so nothing deletes the
// vector when its source row goes: the caller removes both.
//
// Distance is vec0's default, L2. For an embedding model whose outputs are unit
// length it ranks exactly as cosine does. A model that is not can switch the
// column to `distance_metric=cosine` in the migration; nothing here changes.

#pragma once

#include <string>
#include <vector>

struct sqlite3;

namespace yoai {
namespace memory {

// Registers vec0 and the vec_* SQL functions on one connection. Once per
// connection, before anything touches a vec0 table -- including a migration
// that creates one.
bool enable_vectors(sqlite3* db, std::string* err);

// One search result.
struct Hit {
    long long id       = 0;
    double    distance = 0;
};

// Errors are returned, never thrown, for the same reason as in the timeline: it
// sits under a C ABI. A false return always fills `err`.
//
// Not thread-safe on its own. put() runs a savepoint, and two threads opening
// savepoints on one connection interleave them. The lock belongs to whoever
// owns the connection.
class VectorIndex {
public:
    explicit VectorIndex(sqlite3* db) : db_(db) {}

    // Store `v` under `id`, replacing any vector already there. The length must
    // match the column's N; vec0 refuses anything else.
    bool put(long long id, const std::vector<float>& v, std::string* err);

    // Succeeds when there was nothing to remove.
    bool remove(long long id, std::string* err);

    // The `k` stored vectors nearest to `q`, nearest first. Fewer when fewer
    // are stored. `k` must be at least 1.
    bool nearest(const std::vector<float>& q, int k,
                 std::vector<Hit>* out, std::string* err);

private:
    sqlite3* db_;
};

}  // namespace memory
}  // namespace yoai
