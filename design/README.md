# Design

Design source of record, kept outside `harmony/` so it serves the Android app
too when that arrives.

| File | What |
|---|---|
| [palettes.json](palettes.json) | The six themes, exact values |
| [icons.md](icons.md) | Lucide, the rules, and the 24-icon inventory |
| [logo.md](logo.md) | The segmented-ring mark, its geometry, and the wordmark |
| [i18n-status.md](i18n-status.md) | Which of the eleven languages are written, and what is left |
| [embedding.md](embedding.md) | Which embedding model, at what quantization, and how the vectors are stored |
| [ui/](ui/) | The upstream review canvas itself — see below |

Three scripts here write into the app, and are the only thing that should:
`make-arkts-palettes.py` generates the palette table, `make-harmony-icons.py`
the icon assets, `sync-locales.py` the untranslated locale directories and the
status table above. Their outputs are committed, so nothing here is needed to
build; they are needed to *change* any of it, and they check invariants on the
way through — that every palette carries all 19 tokens, that each family is one
light and one dark, that a new palette has been registered.

## The canvas in `ui/`

`ui/` is the Claude Design project "Body Clock", downloaded whole: the canvas
(`Body Clock.dc.html`), its runtime (`support.js`, `image-slot.js`,
`android-frame.jsx`), and `_ds/`, the Tanka design system it is built on. Open
the HTML in a browser. It is a review canvas, not a finished spec: rounds 1, 5,
10, 11 and 16, each carrying several candidates. What is recorded in the other
files here is only what has settled.

All four parts are needed. Without `_ds/` the page still lays out, but headings
fall back to a serif face and six design-system components — three `Button`,
three `MemoryChip`, plus `Switch` and `MessageBubble` in round 1 — do not render
at all. They disappear silently rather than erroring, which is the worst way for
a spec to be missing. The canvas also pulls its icon font from a CDN, so offline
the icons vanish while the layout survives.

### Never take a color off the canvas

Colors come from [palettes.json](palettes.json). The canvas has two sources of
wrong ones:

- **The design-system components carry Tanka's theme**, not the app's. A
  `Button` renders in Tanka's `--primary` (`#0096F8`) where the app's accent is
  `#0072E8` — close enough to look right and be wrong.
- **The inline fallbacks in the markup are a pre-round-10 palette.** The screens
  read `--a-*` variables that the canvas fills in live, but the literals written
  beside them (`var(--a-page, #EEF4FA)`) are the old values. Round 10 moved
  every palette onto one shared neutral ground, so `--a-page` is `#F8F9F8` now.
  Eyedropping a running canvas is fine; reading its source is not.

The screens themselves are driven by the app palette, not by Tanka — inside
round 5 the `--a-*` variables outnumber design-system ones by roughly ten to
one, and what is left of Tanka there is typography and a couple of hairlines.

## What the design covers

The daily-record half of the app — the part that collects the personal data the
model later talks over:

- Home is a **current-state header plus a nine-cell grid**. Tapping a cell does
  not log an event, it *switches the current state*; the previous state ends and
  its duration is written. A day is therefore continuous with no back-filling.
- **Ending** is separate from switching: it closes the current state and opens
  nothing, and the gap that follows is left as a gap. Undo is offered while it
  is still fresh.
- Per-state screens give only that state's input (a meal gets the camera and
  what you ate; a nap gets a dark screen and a timer).
- A day view where states form a single bar and gaps are the only thing that
  needs filling in, plus a month calendar whose cells carry that day's coverage.
- Photos go through the **system camera** — no in-app viewfinder — and come back
  as one confirmation card.
- Settings for palette, language and text size; model settings for a remote key
  or a local file.

**There is no bottom navigation bar.** Every screen carries the same floating
button at its lower right; it opens a stack of pills — Ask, Now/Today, Settings
— and collapses back to a dot. Going back is always that one gesture, so no
screen needs its own back arrow. The vault screen from earlier rounds is gone as
a destination: data and model belong to Settings.

The rules behind the day log — one open entry, switching leaves no gap, a gap is
the absence of a row — are implemented and documented in
[src/store/day_store.hpp](../src/store/day_store.hpp), with the schema in
[sql/sqlite/](../sql/sqlite/).

**Eleven interface languages**: Chinese, English, French, Spanish, German,
Italian, Japanese, Korean, Russian, Arabic, Hebrew. Two of them are RTL, so
**RTL is a requirement, not a later nice-to-have** — the design already uses
logical properties (`inset-inline-start`, `margin-inline-start`) throughout.
The `1g` screen wires up only four of the eleven; that is a demo, not the
scope.
