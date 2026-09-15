# Icons

Source: Claude Design project "Body Clock", round 11.

## The set

**Lucide** — ISC, open source, drawn on a 24px grid with round caps and joins.
It is also what the Tanka design system uses, so the two match without
adjustment.

A pinned copy of the icons we use lives in
[third_party/lucide](../third_party/lucide), and **that copy is the source of
record**. The canvas loads Lucide from a CDN at `@latest`, which is fine for a
review canvas and useless as a reference: it moves.

Not an icon font. A font cannot do two-tone, screen readers read the glyphs as
garbage, and the stroke weight shifts with the font weight instead of staying
put.

## Rules

- Draw on a **24px canvas**, stroke **1.5px**, round cap and round join.
- **Stroke only** — no filled variants, and never mix filled and stroked in the
  same surface.
- **`currentColor` always.** An icon takes its color from the text color around
  it; it never carries a color of its own.
- Render at **24px** in navigation and toolbars, **20px** in list rows,
  **16px** inline and in metadata rows. These were raised twice while the
  px/vp bug below was still in the code, which did nothing but reserve more
  empty space around a third-size mark; both raises are undone. The three
  numbers live in `core/Icons.ets` and are the only place to change them.
- **No circular background** behind an icon, unless the icon *is* a standalone
  tappable button.
- **RTL mirrors direction only** — back, send. Clock and camera do not mirror.

**1.5px, and no per-icon exceptions.** This file previously said 2px, and gave
per-icon weights between 1.75 and 2.6 measured off a design file that is not in
this repo and cannot be checked. Round 11 leaves stroke width as a live control
on the canvas; its default, its stated rule and what it renders are all 1.5.

Two things made 2px look right. Lucide's standalone `.svg` files carry
`stroke-width="2"` — that is the default for a file opened on its own, and the
canvas overrides it when drawing. And the canvas's own fallback paths, seen
when it cannot reach the CDN, are simplified stand-ins drawn to read at a
heavier weight.

One thing that looks like an icon and is not: the small color bar at the top of
each state cell on the home screen. It is a state color from the palette's
`state-ramp`, not a glyph — do not substitute an icon for it.

## Inventory

The design's 24, and the three more the app already draws. **Name** is what the
canvas calls it; **file** is the pinned SVG in
[third_party/lucide/icons](../third_party/lucide/icons), which is what to
transcribe from; **Lucide** is the component name, useful only on a platform
that can import the package. Four of the names differ from the file, so go by
the file.

**✓** means the path data is already in `core/Icons.ets`.

| | Name | File | Lucide |
|---|---|---|---|
| | mic | `mic.svg` | `Mic` |
| | camera | `camera.svg` | `Camera` |
| | plus | `plus.svg` | `Plus` |
| ✓ | check | `check.svg` | `Check` |
| | search | `search.svg` | `Search` |
| | brain | `brain.svg` | `Brain` |
| | send | `send.svg` | `Send` |
| | lock | `lock.svg` | `Lock` |
| | sun | `sun.svg` | `Sun` |
| | moon | `moon.svg` | `Moon` |
| | clock | `clock.svg` | `Clock` |
| | timer | `timer.svg` | `Timer` |
| | list | `list.svg` | `List` |
| | chart | **`bar-chart-3.svg`** | `BarChart3` |
| | image | `image.svg` | `Image` |
| | download | `download.svg` | `Download` |
| ✓ | settings | `settings.svg` | `Settings` |
| | tag | `tag.svg` | `Tag` |
| | trash | **`trash-2.svg`** | `Trash2` |
| ✓ | chevron | **`chevron-right.svg`** | `ChevronRight` |
| | close | **`x.svg`** | `X` |
| | cpu | `cpu.svg` | `Cpu` |
| | globe | `globe.svg` | `Globe` |
| | type | `type.svg` | `Type` |

Three the app draws that the design's set does not name — the drawer button and
the other two chevron directions. Pinned at the same version as the rest:

| | Name | File | Lucide |
|---|---|---|---|
| ✓ | menu | `menu.svg` | `Menu` |
| ✓ | chevron-left | `chevron-left.svg` | `ChevronLeft` |
| ✓ | chevron-down | `chevron-down.svg` | `ChevronDown` |

HarmonyOS has no Lucide package. The icons ship as path data inlined into an
ArkUI `Shape` + `Path`.

They live in `harmony/entry/src/main/ets/core/Icons.ets`, one table plus one
`icon(name, size, color)` builder, and nothing else in the app draws a `Shape`.
Six of the 24 are in that table — `menu`, the three chevrons, `check`,
`settings` — because those are what the screens that exist actually draw; add
the rest when the screen that needs them lands, rather than filling the table
in from memory.

**Path data comes from [third_party/lucide](../third_party/lucide).** This file
used to say the opposite — that where an earlier design file and Lucide
differed, the design file won, citing a `check` of `m5 13 4 4L19 7` at stroke
2.6 against Lucide's `M20 6 9 17l-5-5`. That reasoning does not survive
contact with the canvas: the canvas *is* Lucide. It loads the library live and
falls back to its own simplified paths only when it cannot reach the CDN,
announcing that in the corner when it happens. The shapes anyone reviewed were
Lucide's; the simplified ones were never on screen except offline.

**Take every element from the `.svg`, not just the `d` attributes.** Several
icons carry a `<circle>` or `<rect>` alongside their paths — `settings` is a
gear path plus `<circle cx="12" cy="12" r="3">`, and `clock` is a circle plus
the hands. Reading only `d=` loses those, and loses them quietly.

**A Unicode glyph is not a substitute.** The screens first shipped with
`Text('‹')`, `Text('›')`, `Text('☰')` and `Text('▾')` standing in for back,
chevron, drawer and caret. A glyph's ink fills only a fraction of its em box,
so `Text('‹').fontSize(24)` draws a hairline about 6vp wide inside a 24vp
layout box, and raising the font size only makes the box taller. It happened
to fail the same way as the px/vp bug above and for an unrelated reason, which
is worth knowing: "the icons are too small" had two separate causes at once.

**Not** as `$r('app.media.*')` SVG resources, even though that is the more
obvious HarmonyOS route. An icon's stroke is a palette color, and a resource can
only carry one value per qualifier — `base/` and `dark/` follow light and dark
but know nothing about which of the three families is in force, so a resource
icon is wrong in four of the six palettes.

**A `Path`'s `commands` are in px. Everything else here is in vp.** This is
the one thing to know about drawing an icon on this platform, and getting it
wrong is not subtle: a 24-unit path in a 24vp box fills a third of it on a 3x
screen, and a ninth of its area. That is how the app's icons first shipped —
every icon, every screen, a third of the space reserved for it.

Two things make it hard to spot. It looks like a design problem rather than a
bug, because everything is proportionally correct and merely too small; and
`Shape`'s `viewPort`, which is where you go looking, does not fix it. Whatever
`viewPort` scales, it does not reconcile px path data with a vp frame, so the
sizes written next to the icons had no visible effect and raising them
appeared to do nothing.

`icon()` therefore uses no `Shape` at all. It converts the grid to px with
`vp2px` and bakes the factor into the command string — arc rotations and flags
excepted, which are not lengths — and passes `strokeWidth` as an explicit
`'…px'` string, since a bare number there would be read as vp and come out
three times too heavy. The only geometry ArkUI is asked for is a plain width
and height.
