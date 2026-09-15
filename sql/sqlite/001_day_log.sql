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

CREATE TABLE entries (
  id         INTEGER PRIMARY KEY,
  state_id   TEXT    NOT NULL REFERENCES states(id),
  started_at INTEGER NOT NULL,          -- ms since the Unix epoch, UTC
  ended_at   INTEGER,                   -- NULL means this is the open one
  local_date TEXT    NOT NULL,          -- 'YYYY-MM-DD', as of started_at
  tz_offset  INTEGER NOT NULL           -- minutes east of UTC, as of started_at
);

-- Rule 1 -- at most one entry open -- enforced rather than trusted.
--
-- It has to be a partial index on a CONSTANT. SQLite treats NULLs as distinct
-- in a unique index, so a plain UNIQUE(ended_at) would happily allow two open
-- entries, which is the exact bug this exists to make impossible. Indexing the
-- literal 1 over only the open rows gives them all the same key, so the second
-- one cannot be inserted.
CREATE UNIQUE INDEX entries_one_open ON entries((1)) WHERE ended_at IS NULL;

CREATE INDEX entries_by_day ON entries(local_date, started_at);
CREATE INDEX entries_by_end ON entries(ended_at);

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
