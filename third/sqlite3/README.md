# SQLite

**3.53.4** (`sqlite-amalgamation-3530400`), the official amalgamation, taken
from sqlite.org. Two of the four files in that archive are here: `sqlite3.c` and
`sqlite3.h`. `shell.c` builds the command-line tool, which nothing here needs,
and `sqlite3ext.h` is only for loadable extensions, which the build switches
off.

Nothing in this directory is edited. Treat it as a binary that happens to be
readable: to change SQLite, replace both files wholesale.

## Why it is vendored rather than resolved

Every mobile target ships SQLite, and none of them lets C++ link it. The
Android NDK has never exposed `libsqlite3` as a stable native library, and
HarmonyOS's native side offers its own `OH_Rdb_*` relational store rather than
the sqlite3 C API — so a C++ core has to carry its own copy either way. iOS is
the one exception, and one exception does not buy a dependency mechanism.

Given that a copy travels with us regardless, carrying the source means every
platform compiles the same bytes, from a version pinned by the file itself. It
is the argument this build already makes for llama.cpp, and it costs about 1 MB
per ABI inside `libyoai.so`.

## Compile-time configuration

The flags live in the root `CMakeLists.txt`, on the `yoai_sqlite3` target,
each with the reason it is set. They are `PUBLIC` deliberately: several of them
change what `sqlite3.h` declares, so a translation unit that includes the header
has to be built with the same set.

`SQLITE_ENABLE_FTS5` is **not** set. Full-text search is wanted eventually — the
design's vault screen counts indexed memory units — but turning it on later is a
rebuild, not a migration, so it stays off until something searches.

## Moving to a new version

Download the amalgamation zip for the version you want, check it against the
SHA3-256 that sqlite.org publishes beside it on the download page, and replace
`sqlite3.c` and `sqlite3.h` from the archive. Record the new version at the top
of this file.

For reference, the archive this copy came from is 2,946,650 bytes with SHA3-256
`628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e`.

## License

SQLite is in the public domain. There is no notice to reproduce and no
attribution requirement; this section exists so the next person does not have to
go and check.
