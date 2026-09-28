// store/vector_index.hpp, exercised against an in-memory database.
//
// Two things are being checked. First, that sqlite-vec is compiled in and that
// Db::open registers it -- a build that silently dropped it would only fail
// once a migration tried to create the table. Second, the one behavior this
// layer adds on top of vec0: put() replaces, and a put() that fails leaves the
// old vector where it was.
//
// Needs no arguments and touches no files:
//
//     build/yoai-vector-index-test

#include "store/db.hpp"
#include "store/vector_index.hpp"

#include <sqlite3.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using yoai::store::Db;
using yoai::store::Hit;
using yoai::store::VectorIndex;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

bool did(bool ok, const std::string& err, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s: %s\n", what, err.c_str());
        ++g_failures;
    }
    return ok;
}

std::vector<float> vec4(float a, float b, float c, float d) {
    std::vector<float> v(4);
    v[0] = a; v[1] = b; v[2] = c; v[3] = d;
    return v;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-5; }

int count_rows(sqlite3* db) {
    sqlite3_stmt* s = 0;
    sqlite3_prepare_v2(db, "SELECT count(*) FROM embeddings;", -1, &s, 0);
    int n = -1;
    if (s && sqlite3_step(s) == SQLITE_ROW) {
        n = sqlite3_column_int(s, 0);
    }
    sqlite3_finalize(s);
    return n;
}

}  // namespace

int main() {
    std::string err;

    // --- only Db::open registers vec0 -----------------------------------------
    // Per connection and on purpose, never through sqlite3_auto_extension; this
    // pins that nothing registers it behind the store's back.
    {
        sqlite3* bare = 0;
        sqlite3_open(":memory:", &bare);
        check(sqlite3_exec(bare, "CREATE VIRTUAL TABLE embeddings USING vec0(embedding float[4]);",
                           0, 0, 0) != SQLITE_OK,
              "vec0 is unknown on a connection Db::open did not make");
        sqlite3_close(bare);
    }

    std::shared_ptr<Db> store = Db::open(":memory:", &err);
    if (!did(store != nullptr, err, "Db::open")) {
        return 1;
    }
    sqlite3* db = store->handle();
    // What a migration will do once the embedding model is chosen.
    if (sqlite3_exec(db, "CREATE VIRTUAL TABLE embeddings USING vec0(embedding float[4]);",
                     0, 0, 0) != SQLITE_OK) {
        std::printf("FAIL  create vec0 table: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    VectorIndex ix(store);
    std::vector<Hit> hits;

    // --- nearest first, with L2 distances --------------------------------------
    did(ix.put(1, vec4(1, 0, 0, 0), &err), err, "put 1");
    did(ix.put(2, vec4(0, 1, 0, 0), &err), err, "put 2");
    did(ix.put(3, vec4(0, 0, 3, 0), &err), err, "put 3");

    did(ix.nearest(vec4(0.9f, 0.1f, 0, 0), 3, &hits, &err), err, "nearest");
    check(hits.size() == 3, "three stored, three returned");
    check(hits.size() == 3 && hits[0].id == 1 && hits[1].id == 2 && hits[2].id == 3,
          "ordered nearest first");
    // sqrt(0.1^2 + 0.1^2) and sqrt(0.9^2 + 0.1^2 + 3^2)
    check(!hits.empty() && near(hits[0].distance, std::sqrt(0.02)), "distance is L2");
    check(hits.size() == 3 && near(hits[2].distance, std::sqrt(0.81 + 0.01 + 9.0)),
          "distance is L2, far end");

    did(ix.nearest(vec4(0, 1, 0, 0), 1, &hits, &err), err, "nearest, k = 1");
    check(hits.size() == 1 && hits[0].id == 2 && near(hits[0].distance, 0),
          "k limits the result, and an exact match is distance 0");

    did(ix.nearest(vec4(0, 1, 0, 0), 10, &hits, &err), err, "nearest, k above the count");
    check(hits.size() == 3, "k above the count returns what there is");

    // --- put replaces ----------------------------------------------------------
    did(ix.put(1, vec4(0, 0, 0, 5), &err), err, "put 1 again");
    check(count_rows(db) == 3, "a second put of one id does not add a row");
    did(ix.nearest(vec4(0, 0, 0, 5), 1, &hits, &err), err, "nearest after replace");
    check(hits.size() == 1 && hits[0].id == 1, "the replacement is what is found");

    // --- a failed put leaves the old vector ------------------------------------
    // put() deletes before it inserts. A wrong length fails the insert, and
    // the delete has to roll back with it.
    std::vector<float> three(3, 1.0f);
    check(!ix.put(2, three, &err), "a vector of the wrong length is refused");
    check(count_rows(db) == 3, "the refused put removed nothing");
    did(ix.nearest(vec4(0, 1, 0, 0), 1, &hits, &err), err, "nearest after refused put");
    check(hits.size() == 1 && hits[0].id == 2 && near(hits[0].distance, 0),
          "the old vector is still there, unchanged");

    // --- inside the caller's transaction ---------------------------------------
    sqlite3_exec(db, "BEGIN;", 0, 0, 0);
    did(ix.put(4, vec4(0, 0, 0, -1), &err), err, "put inside BEGIN");
    check(sqlite3_exec(db, "COMMIT;", 0, 0, 0) == SQLITE_OK,
          "put nests in an open transaction and it still commits");
    check(count_rows(db) == 4, "the nested put is committed");

    // --- remove ----------------------------------------------------------------
    did(ix.remove(4, &err), err, "remove 4");
    did(ix.remove(4, &err), err, "remove 4 again, nothing there");
    check(count_rows(db) == 3, "remove deletes one row");

    // --- refused queries -------------------------------------------------------
    check(!ix.nearest(vec4(0, 1, 0, 0), 0, &hits, &err), "k = 0 is refused");
    check(!ix.nearest(std::vector<float>(), 1, &hits, &err), "an empty query is refused");
    check(!ix.put(5, std::vector<float>(), &err), "an empty vector is refused");

    std::printf("\n%s\n", g_failures ? "FAILED" : "all ok");
    return g_failures ? 1 : 0;
}
