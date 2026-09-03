# Design

Design source of record, kept outside `harmony/` so it serves the Android app
too when that arrives.

| File | What |
|---|---|
| [palettes.json](palettes.json) | The six themes, exact values |
| [icons.md](icons.md) | Lucide, the rules, and the 24-icon inventory |
| [logo.md](logo.md) | The segmented-ring mark, its geometry, and the wordmark |
| [i18n-status.md](i18n-status.md) | Which of the eleven languages are written, and what is left |

Three scripts here write into the app, and are the only thing that should:
`make-arkts-palettes.py` generates the palette table, `make-harmony-icons.py`
the icon assets, `sync-locales.py` the untranslated locale directories and the
status table above.

Upstream is the Claude Design project **"YoAI"**
(`claude.ai/design/p/eb024405-8fa1-4804-9672-1e9eafc6e05f`), built on the Tanka
design system. It is a review canvas, not a finished spec: rounds 1, 5, 10, 11
and 16, each carrying several candidates. What is recorded here is only what has
settled.

## What the design covers

The daily-record half of the app — the part that collects the personal data the
model later talks over, which the README's status table still lists as "local
personal-data storage, not yet":

- Home is a **current-state header plus a nine-cell grid**. Tapping a cell does
  not log an event, it *switches the current state*; the previous state ends and
  its duration is written. A day is therefore continuous with no back-filling.
- Per-state screens give only that state's input (a meal gets the camera and
  what you ate; a nap gets a dark screen and a timer).
- A day view where states form a single bar, and gaps are the only thing that
  needs filling in.
- Photos go through the **system camera** — no in-app viewfinder — and come back
  as one confirmation card.
- Settings for palette, language and text size; model settings for a remote key
  or a local file.

The chat UI stays as it is today. The design's `1e` Ask screen is a chat over
your own entries, which is a different surface from the model chat YoAI already
has.

**Eleven interface languages**: Chinese, English, French, Spanish, German,
Italian, Japanese, Korean, Russian, Arabic, Hebrew. Two of them are RTL, so
**RTL is a requirement, not a later nice-to-have** — the design already uses
logical properties (`inset-inline-start`, `margin-inline-start`) throughout.
The `1g` screen wires up only four of the eleven; that is a demo, not the
scope.

## Two things to settle before implementing

**Six themes do not fit HarmonyOS resource qualifiers.** Today
[color.json](../harmony/entry/src/main/resources/base/element/color.json) exists
twice, under `base/` and `dark/`, and the system picks between them. Six named
themes the user chooses from cannot work that way — they have to be runtime
state in ArkTS, with the resource files reduced to a default. That is a
structural change to how the app reads color, not a value swap.

**The palettes cover 10 tokens; the app defines 16.** These map directly:

| palettes.json | color.json |
|---|---|
| `surface-page` | `bg_page` |
| `surface-card` | `bg_surface` |
| `text-primary` | `text_primary` |
| `text-tertiary` | `text_tertiary` |
| `border` | `border` |
| `accent` | `brand` |

`accent-soft`, `surface-inverse`, `muted-on-inverse`, `accent-on-inverse` and
the four-color `state-ramp` are new — the ramp and the inverse surface are what
the current-state header and the state cells are built from.

Nothing in the six palettes supplies `bg_bubble`, `bg_overlay`, `text_secondary`,
`text_disabled`, `divider`, `scrim`, `on_brand`, `danger`, `success` or
`start_window_background`. Those have to be derived per theme or decided
separately; six themes means six answers, not one.
