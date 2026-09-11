# Logo

Source: Claude Design project, round 16. Option `16d` is final; `13c` is the
horizontal wordmark that goes with it.

## The mark

A **segmented ring** around a **standing figure** — the day, cut into arcs, one
arc per state, the current state's arc in the accent color; the figure is whose
day it is. Clock hands sit over the figure's chest. A blue plus at the lower
right means "record one". The whole thing sits on a rounded tile of its own.

Four decisions worth keeping:

- **The ring is many arcs, never one.** A single arc with a gap reads as a power
  button. Multiple segments read as a clock face.
- **Full circle plus visible hands = a clock.** That is what stops the mark
  being read as a toggle.
- **The figure is a shadow layer.** One flat neutral at low opacity, no outline,
  no features. It gives the ring something to be about without competing with
  it — at small sizes it drops out entirely (see below) and the mark still
  works.
- **The mark carries no text.** The wordmark is the words `Body Clock`, set
  separately. One asset set serves all eleven languages.

## Geometry

On a 96-unit canvas, with the ring radius `r = 41`:

| Element | Rule |
|---|---|
| Tile | full canvas, corner radius 22 |
| Ring | `r = 41`, stroke 11, round caps |
| Figure | clipped to a circle of `r = 33` about the center — head clear of the top arc, feet on the circle's lower edge |
| Hand axis | `(48, 44)` — above center, so the hands cross the chest and not the face |
| Plus center | `cx + 0.63r`, `cy + 0.63r` (each axis — i.e. out along 45°) |
| Plus disc | `0.37r`, solid accent, laid straight over the ring |

The ring is drawn with four `<circle>` elements sharing a center, each with a
`stroke-dasharray`/`stroke-dashoffset` pair cutting one arc, all inside
`transform="rotate(-90 cx cy)"` so the first segment starts at twelve o'clock.

The ring's bounding box is the mark's bounding box: the four segments between
them cover twelve, three, six and nine o'clock, so the mark is square and
centered on the ring. The plus badge reaches farther from the center than the
ring does — it, not the ring, is what a round mask clips first.

**At small sizes drop to three segments and lose the figure.** Four arcs plus
their gaps stop resolving below roughly 40px, and the figure turns to mush well
before that. The icon assets stay on the four-segment mark whatever size the
launcher happens to draw them at.

## The two variants

Drawn in the default theme's colors — `daylight-arctic` and `midnight-arctic`
from [palettes.json](palettes.json), except the tile, which is the design's own
app-icon ground:

| | Light | Dark |
|---|---|---|
| Tile | `#E7EEF6` | `#16233A` |
| Ring, hands | `#08131F` | `#EAF3FB` |
| Current segment, plus disc | `#0072E8` | `#4FB2FF` |
| Figure | `#4E6478` at 32% | `#7C92A8` at 36% |
| Plus glyph | `#FAFDFF` | `#0B1622` |
| Page ground around the mark | `#FAFDFF` | `#0B1622` |

The plus has two treatments. **Filled** — a solid accent disc with a light plus
on it, laid straight over the ring with nothing under it — is the default, and
it is what the icon assets use. At this weight the disc reads cleanly against
the arc it covers, so it needs no ground-colored washer to separate the two.
**Outlined** — 1.5px stroke, ground-colored fill — is what the smaller wordmark
ring uses, where a filled disc would swallow too much of the ring.

## The drawing (96 × 96)

Four segments — the icon assets and every normal use. The figure's own path is
long and is not repeated here; take it from
[make-harmony-icons.py](make-harmony-icons.py), which holds the one copy.

```svg
<svg width="96" height="96" viewBox="0 0 96 96" fill="none">
  <rect width="96" height="96" rx="22" fill="#E7EEF6"/>
  <defs><clipPath id="inner"><circle cx="48" cy="48" r="33"/></clipPath></defs>
  <g opacity=".32" fill="#4E6478" clip-path="url(#inner)">
    <path transform="translate(38.5 15) scale(0.33)" d="M23.5 0.0L34.1 0.0 … Z"/>
  </g>
  <g transform="rotate(-90 48 48)" fill="none" stroke-linecap="round" stroke-width="11">
    <circle cx="48" cy="48" r="41" stroke="#08131F" stroke-dasharray="68 190" opacity=".9"/>
    <circle cx="48" cy="48" r="41" stroke="#08131F" stroke-dasharray="39 219" stroke-dashoffset="-78" opacity=".5"/>
    <circle cx="48" cy="48" r="41" stroke="#08131F" stroke-dasharray="53 205" stroke-dashoffset="-125" opacity=".28"/>
    <circle cx="48" cy="48" r="41" stroke="#0072E8" stroke-dasharray="59 199" stroke-dashoffset="-186"/>
  </g>
  <path d="M48 44V34M48 44h9" stroke="#08131F" stroke-width="2.4" stroke-linecap="round"/>
  <circle cx="48" cy="44" r="2.6" fill="#08131F"/>
  <circle cx="74" cy="74" r="15" fill="#0072E8"/>
  <path d="M74 67v14M67 74h14" stroke="#FAFDFF" stroke-width="2.8" stroke-linecap="round"/>
</svg>
```

Dark: swap `#E7EEF6` → `#16233A`, `#08131F` → `#EAF3FB`, `#0072E8` → `#4FB2FF`,
`#4E6478`/.32 → `#7C92A8`/.36, and the plus glyph `#FAFDFF` → `#0B1622`. The two
dimmed segments also drop a step — .5/.28 becomes .45/.26 — because light ink on
a dark ground carries further than the reverse.

Three-segment version for small sizes — no figure, hands back on center and
longer, ring opened up to `r=38` with stroke 18, and everything else scaled up
to match:

```svg
<svg width="96" height="96" viewBox="0 0 96 96" fill="none">
  <rect width="96" height="96" rx="22" fill="#E7EEF6"/>
  <g transform="rotate(-90 48 48)" fill="none" stroke-linecap="round" stroke-width="18">
    <circle cx="48" cy="48" r="38" stroke="#08131F" stroke-dasharray="63 176" opacity=".9"/>
    <circle cx="48" cy="48" r="38" stroke="#08131F" stroke-dasharray="45 194" stroke-dashoffset="-76" opacity=".45"/>
    <circle cx="48" cy="48" r="38" stroke="#0072E8" stroke-dasharray="70 169" stroke-dashoffset="-138"/>
  </g>
  <path d="M48 48V31M48 48h12" stroke="#08131F" stroke-width="5.5" stroke-linecap="round"/>
  <circle cx="48" cy="48" r="3.4" fill="#08131F"/>
  <circle cx="74" cy="74" r="17" fill="#0072E8"/>
  <path d="M74 66.5v15M66.5 74h15" stroke="#FAFDFF" stroke-width="4" stroke-linecap="round"/>
</svg>
```

## Horizontal wordmark (`13c`)

For the website and the tablet sidebar: `B` + the ring as the `o` + `dy Clock`.
The letters are 600 weight, `letter-spacing: -0.03em`, in the UI font. Minimum
height 18px.

The ring here is a smaller, older drawing — `viewBox="0 0 48 48"`, `r = 16`,
stroke 3, no figure and no tile, with the **outlined** plus at the original
`0.78r` / `0.42r` proportion, so it lands at `(33.5, 33.5)` with radius `6.7`.
That proportion is the wordmark's alone; the icon mark above is on `0.63r` /
`0.37r`. Do not "unify" them by moving one — they were fitted to different ring
weights.

The app-icon slot always uses the `16d` mark, never the wordmark.

## The HarmonyOS assets

[make-harmony-icons.py](make-harmony-icons.py) regenerates all four from the
four-segment mark: `app_icon.png`, the layered icon's `foreground.png` and
`background.png`, and `startIcon.png`. Content is 682px on a 1024 canvas.

The design draws the ring pressing against the edges of its own rounded tile.
The tile's edge and a launcher mask's edge are not the same thing, so the assets
press against the mask instead: since the mark's bounding box is exactly the
ring's, 682px of content puts the ring's outer edge precisely on the 341px
radius of the conventional two-thirds circular safe zone — the strictest mask
any launcher applies. That is the largest ring that survives every mask intact.

The plus badge then crosses that circle, at 380px against 341. HarmonyOS masks
with a squircle, which is much more generous at 45° — exactly where the badge
sits — so nothing is actually lost there, and against a strict circular mask the
badge's outer edge would be shaved, which costs far less than clipping the ring.

The cost of staying inside the safe zone is that its margin is visible: on a
device the mark reads as sitting on a near-white plate rather than filling the
icon. Reproducing the design 1:1 instead — 992px of content, ring 16px from the
canvas edge — removes that, and the ring still clears both a squircle and a
circle inscribed in the canvas, since it is concentric with either. That was
tried and rolled back. The assets stay at 682.

In the layered icon the tile becomes `background.png`, a flat `#E7EEF6`, and the
mark becomes `foreground.png`. Nothing in the mark is filled with the tile
color, so the two layers do not have to agree on anything and the foreground
stays correct over any background.

`startIcon.png` carries its own ground rather than being transparent over
`$color:start_window_background` — that color is `#101315` in the dark theme,
and the mark's `#08131F` arcs would all but vanish on it.

## Open

The segments are currently opacity steps of the ink color, not real state
colors. Using the palette's four-color `state-ramp` instead is on the design's
own list of next steps and has not been decided.
