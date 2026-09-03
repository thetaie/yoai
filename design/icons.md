# Icons

Source: Claude Design project "YoAI 有爱", round 11.

## The set

[Lucide](https://lucide.dev) — MIT, open source, drawn on a 24px grid with round
caps and joins. It is also what the Tanka design system uses, so the two match
without adjustment.

Not an icon font. A font cannot do two-tone, screen readers read the glyphs as
garbage, and the stroke weight shifts with the font weight instead of staying
put.

## Rules

- Draw on a **24px canvas**, stroke **1.5px**, round cap and round join.
- **Stroke only** — no filled variants, and never mix filled and stroked in the
  same surface.
- **`currentColor` always.** An icon takes its color from the text color around
  it; it never carries a color of its own.
- Render at **22px** in navigation and toolbars, **16px** inline and in
  metadata rows. The design draws them at 20–22px in the screens.
- **No circular background** behind an icon, unless the icon *is* a standalone
  tappable button.
- **RTL mirrors direction only** — back, send. Clock and camera do not mirror.

The 1.5px stroke is the design's proposed default, not yet confirmed across all
screens; round 11 left it as a live control so it could be compared at 1, 1.5, 2
and 2.5.

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

HarmonyOS has no Lucide package. The icons ship as Lucide's own path data
inlined into an ArkUI `Shape` + `Path`, whose `viewPort` does the job an SVG
`viewBox` would. The design file carries a hand-drawn fallback path for each of
the 24, used when Lucide fails to load; those are a starting point, but the real
Lucide paths are the ones to ship.

**Not** as `$r('app.media.*')` SVG resources, even though that is the more
obvious HarmonyOS route. An icon's stroke is a palette color, and a resource can
only carry one value per qualifier — `base/` and `dark/` follow light and dark
but know nothing about which of the three families is in force, so a resource
icon is wrong in four of the six palettes.

**The stroke scales with the frame.** `Shape` scales its contents from the
`viewPort` to the rendered size, stroke width included, so a 24 viewPort drawn
at 18vp turns a `strokeWidth(1.5)` into 1.125 on screen. To get the 1.5 above,
write `1.5 x viewPort / rendered` — 2 at 18vp, 1.64 at 22vp — or keep the
viewPort 1:1 with the size and write 1.5. Both mistakes are silent and just look
faint.
