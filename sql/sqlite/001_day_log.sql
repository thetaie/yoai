-- The day log: states with a start and an end, and the notes hung off them.
--
-- Applied once, when the database is at version 0. Nothing here is defensive --
-- no IF NOT EXISTS -- because a migration that has already run is not run
-- again, and a statement that fails should say so rather than shrug.
--
-- See src/store/day_store.hpp for the five rules this schema exists to hold up.

-- There is no meta/settings table on purpose. The one thing it would have held
-- is the schema version, and that belongs in PRAGMA user_version: meta would
-- itself be created by this migration, so asking it which migrations have run
-- means depending on the schema it is supposed to describe. If a key/value need
-- turns up later it can arrive as its own migration, with a reader.

-- The nine built-ins are seeded below. Ids are stable tokens and are NEVER
-- shown: the names the user reads are string resources, so they follow the
-- interface language. This is a table rather than an enum because custom states
-- are on the design's list, and they will be rows here.
CREATE TABLE states (
  id       TEXT PRIMARY KEY,
  position INTEGER NOT NULL,
  builtin  INTEGER NOT NULL DEFAULT 0
);

-- Times are milliseconds since the Unix epoch, UTC. Each endpoint carries the
-- UTC offset in force AT THAT ENDPOINT, in minutes east.
--
-- Both, not one: to place an entry on a 00-24 bar of local time you convert
-- each endpoint with the offset that was true when it happened, and a user who
-- flies -- or who crosses a daylight-saving switch -- ends a stretch in a
-- different offset than they started it. One shared offset would misplace the
-- end by the whole difference, which for a Beijing-to-London flight is seven
-- hours.
--
-- An offset is stored rather than a timezone NAME because timezone rules get
-- amended retroactively. An offset is what was actually true at that instant
-- and no later tzdb update can rewrite it. There is also no portable tzdb under
-- C++ on either mobile target, which settles it from the other side.
--
-- The local day is DERIVED, never stored by hand -- a date that can disagree
-- with the timestamp it came from is a bug waiting to be written. It is a day
-- number (days since the epoch) rather than 'YYYY-MM-DD' for two reasons: the
-- month view and the "last 7 days" lines are range and subtraction rather than
-- string building, and the expression stays plain integer arithmetic instead of
-- reaching for date(), whose modifier family includes non-deterministic ones
-- that a generated column may not use at all.
--
-- To read one back by hand: date(local_day * 86400, 'unixepoch').
CREATE TABLE entries (
  id            INTEGER PRIMARY KEY,
  state_id      TEXT    NOT NULL REFERENCES states(id),
  started_at    INTEGER NOT NULL,
  started_tz    INTEGER NOT NULL,
  ended_at      INTEGER,                 -- NULL means this is the open one
  ended_tz      INTEGER,                 -- NULL exactly when ended_at is
  local_day     INTEGER GENERATED ALWAYS AS
                  ((started_at + started_tz * 60000) / 86400000) VIRTUAL,
  local_day_end INTEGER GENERATED ALWAYS AS
                  ((ended_at + ended_tz * 60000) / 86400000) VIRTUAL
);

-- Rule 1 -- at most one entry open -- enforced rather than trusted.
--
-- It has to be a partial index on a CONSTANT. SQLite treats NULLs as distinct
-- in a unique index, so a plain UNIQUE(ended_at) would happily allow two open
-- entries, which is the exact bug this exists to make impossible. Indexing the
-- literal 1 over only the open rows gives them all the same key, so the second
-- one cannot be inserted.
CREATE UNIQUE INDEX entries_one_open ON entries((1)) WHERE ended_at IS NULL;

-- Two day indexes because a day is asked about from both ends: an entry belongs
-- to the day it STARTED in, but it also has to appear on the day it ENDED in,
-- or the night's sleep leaves the next morning looking like a gap.
CREATE INDEX entries_by_day ON entries(local_day, started_at);
CREATE INDEX entries_by_day_end ON entries(local_day_end);

-- ended_at carries the single most frequent query in the app: "what just
-- stopped", which every screen refresh and every write asks for as
-- ORDER BY ended_at DESC LIMIT 1. With this index that walks one entry and
-- stops; without it, it is a full scan plus a temporary B-tree to sort, on a
-- table that only grows. entries_one_open does not help -- it covers the open
-- row, which is the one this query excludes.
CREATE INDEX entries_by_end ON entries(ended_at);

-- started_at is what every listing orders by, and an ORDER BY with no usable
-- index is the same temporary B-tree again. Not merged into entries_by_day:
-- the day listings filter on a derived column and order on this one, so they
-- need it standing on its own.
CREATE INDEX entries_by_start ON entries(started_at);

CREATE TABLE notes (
  id       INTEGER PRIMARY KEY,
  entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
  text     TEXT    NOT NULL,
  at       INTEGER NOT NULL
);

CREATE INDEX notes_by_entry ON notes(entry_id, at);

INSERT INTO states(id, position, builtin) VALUES
  ('work',      0, 1),
  ('eat',       1, 1),
  ('nap',       2, 1),
  ('commute',   3, 1),
  ('exercise',  4, 1),
  ('read',      5, 1),
  ('social',    6, 1),
  ('rest',      7, 1),
  ('childcare', 8, 1);
