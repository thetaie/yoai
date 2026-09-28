// The app's one database, and its one connection.
//
// yoai.db holds everything the core persists: the timeline's tables, the
// vectors, and what comes after them. Every module in store/ is a view over
// one Db, never an owner of a connection of its own.
//
// That is the reason this class exists. Two SQLite connections in one process,
// each with its own locks on one file, is the failure that makes linking the
// platform's SQLite from native code a data-corruption bug on Android -- and it
// is also how two modules end up waiting on each other's write locks. So the
// connection lives here once, and a module holds a shared_ptr to it.
//
// Opening it does everything the file needs before any module touches it:
//
//   1. WAL, synchronous=NORMAL and a busy timeout -- see open() for why.
//   2. sqlite-vec registered, so a migration can create a vec0 table.
//   3. Every migration in sql/sqlite/ newer than the file, in order.
//
// Tables are unprefixed. The file is the namespace, and a module prefix would
// claim `entries` for the timeline when everything else hangs off it.

#pragma once

#include <memory>
#include <mutex>
#include <string>

struct sqlite3;

namespace yoai {
namespace store {

class Db {
public:
    // `dir`/yoai.db -- the file every host opens. `dir` must already exist: it
    // is the host's sandbox directory, and creating it is the host's business.
    static std::string path_in(const std::string& dir);

    // Opens (creating if needed) the database at `path` and brings it up to
    // date, as described above. ":memory:" gives a private in-memory database,
    // which is what the tests use.
    //
    // Returns null on failure, with `err` saying why. Errors are returned,
    // never thrown: this sits directly under a C ABI.
    static std::shared_ptr<Db> open(const std::string& path, std::string* err);

    ~Db();

    // The connection. Serialized mode (SQLITE_THREADSAFE=1) makes any single
    // call safe from any thread.
    sqlite3* handle() const { return db_; }

    // Held by every module for the length of anything that is more than one
    // statement -- a transaction, a savepoint, a read that has to agree with
    // itself. One lock for the connection rather than one per module, because
    // two modules each holding their own would interleave transactions on it.
    std::mutex& lock() { return lock_; }

    const std::string& path() const { return path_; }

    // PRAGMA user_version: the last migration applied, 0 for a new file.
    int schema_version() const;

private:
    Db(sqlite3* db, const std::string& path) : db_(db), path_(path) {}
    Db(const Db&);
    Db& operator=(const Db&);

    sqlite3*    db_;
    std::string path_;
    std::mutex  lock_;
};

}  // namespace store
}  // namespace yoai
