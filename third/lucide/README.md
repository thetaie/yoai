# Lucide

**1.46.0**, ISC. Twenty-four icons from the `lucide-static` package, one SVG
each, plus the upstream LICENSE.

Every file carries its own `@license lucide-static v1.46.0` comment, so the
version travels with the artwork and cannot drift away from this note.

## Why these files are here

The design canvas pulls Lucide from a CDN at `@latest`. That is right for a
review canvas and wrong for a source of record: `@latest` moves, and Lucide has
renamed and redrawn icons across releases. Pinning a copy means the shapes the
design was reviewed against are the shapes we ship, and that adding the
twenty-fifth icon later does not silently arrive in a newer style than the
first twenty-four.

Only the icons the design actually uses are here. Adding one means fetching
that one file at this same version — not vendoring the whole set.

## These are the source, not the build input

Nothing reads this directory at build time. The app draws icons from a table of
path strings in `examples/harmony/entry/src/main/ets/core/Icons.ets`, transcribed from
these files as each screen needs them. This directory is what that table is
checked against, and what proves where the shapes came from.

`stroke-width="2"` in these files is Lucide's own default for its standalone
SVGs. It is **not** the app's icon weight, which is decided in
[design/icons.md](../../design/icons.md) and applied when drawing.

## Canvas name to Lucide name

The design canvas uses its own short names. Four of them do not match Lucide's:

| Canvas | Lucide |
|---|---|
| `chart` | `bar-chart-3` |
| `close` | `x` |
| `chevron` | `chevron-right` |
| `trash` | `trash-2` |

The other twenty are the same word in both.

## Updating

Fetch the icons you want from the `lucide-static` package at a single pinned
version — all of them from the same one — and replace the files wholesale, then
record the new version at the top of this file. Do not mix versions: that is
the drift this directory exists to prevent.

Changing version means re-checking the transcribed paths in `Icons.ets` against
the new files. Nothing warns you if they diverge, because nothing reads these
files at build time.
