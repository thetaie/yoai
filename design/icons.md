# Icons

Source: Claude Design project "Body Clock", round 11.

## The set

[Lucide](https://lucide.dev) — MIT, open source, drawn on a 24px grid with round
caps and joins. It is also what the Tanka design system uses, so the two match
without adjustment.

Not an icon font. A font cannot do two-tone, screen readers read the glyphs as
garbage, and the stroke weight shifts with the font weight instead of staying
put.

## Rules

- Draw on a **24px canvas**, stroke **2px**, round cap and round join.
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

2px settles what round 11 left as a live control. 1.5 was the proposal, but it
is not what the design file draws: every icon in `bodyclock-design` is between 1.75
and 2.6 — plus 2.2, lock 1.9–2, camera 1.75, and the selected-row tick 2.6.

The tick keeps the design's own 2.6: it is the one icon that has to read as a
state at a glance rather than as decoration. Per-icon weights live beside the
path data in `core/Icons.ets`.

One thing that looks like an icon and is not: the small color bar at the top of
each state cell on the home screen. It is a state color from the palette's
`state-ramp`, not a glyph — do not substitute an icon for it.

## Inventory

24 icons, with the Lucide component name to import.

| Name | Lucide |
|---|---|
| mic | `Mic` |
| camera | `Camera` |
| plus | `Plus` |
| check | `Check` |
| search | `Search` |
| brain | `Brain` |
| send | `Send` |
| lock | `Lock` |
| sun | `Sun` |
| moon | `Moon` |
| clock | `Clock` |
| timer | `Timer` |
| list | `List` |
| chart | `BarChart3` |
| image | `Image` |
| download | `Download` |
| settings | `Settings` |
| tag | `Tag` |
| trash | `Trash2` |
| chevron | `ChevronRight` |
| close | `X` |
| cpu | `Cpu` |
| globe | `Globe` |
| type | `Type` |

HarmonyOS has no Lucide package. The icons ship as path data inlined into an
ArkUI `Shape` + `Path`.

They live in `harmony/entry/src/main/ets/core/Icons.ets`, one table plus one
`icon(name, size, color)` builder, and nothing else in the app draws a `Shape`.
Six of the 24 are in that table — `menu`, the three chevrons, `check`,
`settings` — because those are what the screens that exist actually draw; add
the rest when the screen that needs them lands, rather than filling the table
in from memory.

**Where the design file and Lucide differ, the design file wins.** Its `check`
is `m5 13 4 4L19 7` at stroke 2.6, which sits lower and heavier than Lucide's
`M20 6 9 17l-5-5` at 1.5, and the heavier one is what the screens were signed
off on. Take path data and weight from `bodyclock-design`; fall back to Lucide only
for icons the design file does not draw.

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
