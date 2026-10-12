# sqlite-vec

**v0.1.9**, the amalgamation release asset
`sqlite-vec-0.1.9-amalgamation.zip` from the asg017/sqlite-vec repository on
GitHub, built from commit `e9f598abfa0c06b328d8fe5da9c3760cce74be10`
(2026-03-31). Both files in that archive are here, unchanged: `sqlite-vec.c` and
`sqlite-vec.h`. The two licenses come from the repository at the same tag.

Nothing in this directory is edited. To change sqlite-vec, replace both files
wholesale and record the new version at the top of this file. The newer
`0.1.10-alpha.*` releases were passed over on purpose: the vectors live in the
user's one database, so this takes stable releases only.

For reference, the archive is 54,035 bytes with SHA-256
`b87cdda12112657ba5ab8842f0088a4090982eaf41f22b2bd6d495b81765a8c9`.

## What it gives us

A `vec0` virtual table: rows of fixed-length float vectors keyed by rowid, and
a K-nearest-neighbor query over them in plain SQL. Search is brute force, which
is the right trade at the scale of one person's memories: ten thousand
1024-dimension vectors are about 40 MB to scan, a few milliseconds on a phone,
and there is no index to build or keep in step. See `src/store/vector_index`
for the code that uses it.

## How it is built

Compiled into the core as the `yoai_sqlite_vec` target in the root
`CMakeLists.txt`, not loaded at runtime. `SQLITE_OMIT_LOAD_EXTENSION` stays on
for SQLite; a connection gets the extension by calling `sqlite3_vec_init`
directly (see `src/store/db.cpp`).

The flags are on that target, each with its reason. Two things worth knowing
before touching them:

- **musl needs `sys/types.h` forced in.** `sqlite-vec.c` defines `uint8_t` from
  `u_int8_t` on every non-Windows target without including the header that
  declares it. glibc and bionic happen to pull it in through the standard
  headers the file does include; musl, which is HarmonyOS's C library, does
  not, and the file fails to compile.
- **SIMD is off.** `SQLITE_VEC_ENABLE_NEON` and `SQLITE_VEC_ENABLE_AVX` speed up
  the distance functions. NEON compiles for arm64 Android, but nothing has
  measured whether it matters at our scale, so it stays off until something
  does.

## License

Dual-licensed, MIT or Apache-2.0, at our choice. Both texts are here as
`LICENSE-MIT` and `LICENSE-APACHE`; either one wants its notice kept with the
source, which this directory does.
