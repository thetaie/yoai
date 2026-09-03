# Logo

Source: Claude Design project "YoAI", round 16. Option `16c` is final; `13c` is
the horizontal wordmark that goes with it.

## The mark

A **segmented ring** — the day, cut into arcs, one arc per state, the current
state's arc in the accent color. Clock hands in the middle. A blue plus at the
lower right, meaning "record one".

Three decisions worth keeping:

- **The ring is many arcs, never one.** A single arc with a gap reads as a power
  button. Multiple segments read as a clock face.
- **Full circle plus visible hands = a clock.** That is what stops the mark
  being read as a toggle.
- **The mark carries no text.** The wordmark is the letters `YoAI`, set
  separately. One asset set serves all eleven languages.

## Geometry

Everything derives from the ring radius `r`, so the mark can be redrawn at any
size:

| Element | Rule |
|---|---|
| Plus center | `cx + 0.78r`, `cy + 0.78r` (each axis — i.e. out along 45°) |
| Plus radius | `0.42r` |

This holds in both sizes the design ships: at `viewBox="0 0 96 96"` with `r=36`
the plus lands at `(76, 76)` with radius `15`; at `viewBox="0 0 48 48"` with
`r=16` it lands at `(33.5, 33.5)` with radius `6.7`.

The ring is drawn with four `<circle>` elements sharing a center, each with a
`stroke-dasharray`/`stroke-dashoffset` pair cutting one arc, all inside
`transform="rotate(-90 cx cy)"` so the first segment starts at twelve o'clock.
Caps are round.

**At small sizes drop to three segments.** Four arcs plus their gaps stop
resolving below roughly 40px. This is for thumbnails and other small
renderings — the icon assets stay on four segments whatever size the launcher
happens to draw them at.

## The two variants

Drawn in the default theme's colors — `daylight-arctic` and `midnight-arctic`
from [palettes.json](palettes.json):

| | Light | Dark |
|---|---|---|
| Ring, hands | `#08131F` | `#EAF3FB` |
| Current segment, plus | `#0072E8` | `#4FB2FF` |
| Ground | `#FAFDFF` | `#0B1622` |

The plus has two treatments. **Outlined** — 1.5px stroke, ground-colored fill,
so it punches a hole in the ring — is the default, and it is what the icon
assets use. **Filled** — a solid accent disc with a ground-colored plus — goes
with the three-segment reduction below, where a thin outline would not hold.

## The drawing (96 × 96)

Four segments — the icon assets and every normal use:

```svg
<svg width="96" height="96" viewBox="0 0 96 96" fill="none">
  <g transform="rotate(-90 48 48)" fill="none" stroke-linecap="round" stroke-width="6">
    <circle cx="48" cy="48" r="36" stroke="#08131F" stroke-dasharray="60 166" opacity=".9"/>
    <circle cx="48" cy="48" r="36" stroke="#08131F" stroke-dasharray="34 192" stroke-dashoffset="-68" opacity=".55"/>
    <circle cx="48" cy="48" r="36" stroke="#08131F" stroke-dasharray="46 180" stroke-dashoffset="-110" opacity=".3"/>
    <circle cx="48" cy="48" r="36" stroke="#0072E8" stroke-dasharray="52 174" stroke-dashoffset="-164"/>
  </g>
  <path d="M48 48V32M48 48h12" stroke="#08131F" stroke-width="2.5" stroke-linecap="round"/>
  <circle cx="48" cy="48" r="3.2" fill="#08131F"/>
  <circle cx="76" cy="76" r="15" fill="#FAFDFF" stroke="#0072E8" stroke-width="1.5"/>
  <path d="M76 69v14M69 76h14" stroke="#0072E8" stroke-width="1.5" stroke-linecap="round"/>
</svg>
```

Dark: swap `#08131F` → `#EAF3FB`, `#0072E8` → `#4FB2FF`, `#FAFDFF` → `#0B1622`.

Three-segment version for small sizes, with the filled plus:

```svg
<svg width="96" height="96" viewBox="0 0 96 96" fill="none">
  <g transform="rotate(-90 48 48)" fill="none" stroke-linecap="round" stroke-width="13">
    <circle cx="48" cy="48" r="33" stroke="#08131F" stroke-dasharray="55 152" opacity=".9"/>
    <circle cx="48" cy="48" r="33" stroke="#08131F" stroke-dasharray="40 167" stroke-dashoffset="-66" opacity=".45"/>
    <circle cx="48" cy="48" r="33" stroke="#0072E8" stroke-dasharray="60 147" stroke-dashoffset="-120"/>
  </g>
  <path d="M48 48V33M48 48h11" stroke="#08131F" stroke-width="6" stroke-linecap="round"/>
  <circle cx="74" cy="74" r="14" fill="#0072E8"/>
  <path d="M74 67v14M67 74h14" stroke="#FAFDFF" stroke-width="6" stroke-linecap="round"/>
</svg>
```

## Horizontal wordmark (`13c`)

For the website and the tablet sidebar: `Y` + the ring as the `o` + `AI`. The
letters are 600 weight, `letter-spacing: -0.04em`, in the UI font. Minimum
height 18px. The ring here is the 48-viewBox drawing above.

The app-icon slot always uses the `16c` mark, never the wordmark.

## The HarmonyOS assets

[make-harmony-icons.py](make-harmony-icons.py) regenerates all four from the
four-segment mark: `app_icon.png`, the layered icon's `foreground.png` and
`background.png`, and `startIcon.png`. Content is 620px on a 1024 canvas.

That size is a deliberate trade. The mark's plus badge sits at 45°, and its
outer edge reaches farther from center than the ring does — so it, not the ring,
is what a launcher mask clips first. The app's original icons used 547px, which
put the farthest pixel exactly on the 341px radius of the conventional
two-thirds circular safe zone, and read small as a result. At 620px the ring
stays well inside that circle and only the badge crosses it; HarmonyOS masks
with a squircle, which is much more generous at 45°, so nothing is actually lost
there. Past roughly 740px the ring itself breaks out and no mask shape saves it.

The ground there is `#EAF3FB`, not the `#FAFDFF` in the table above: `#FAFDFF`
is the page ground the mark sits on inside the product, while `#EAF3FB` is what
the design's own app-icon tile uses. The plus is filled with the same `#EAF3FB`
so it still punches a hole.

`startIcon.png` carries its own ground rather than being transparent over
`$color:start_window_background` — that color is `#101315` in the dark theme,
and the mark's `#08131F` arcs would all but vanish on it.

## Open

The segments are currently opacity steps of the ink color, not real state
colors. Using the palette's four-color `state-ramp` instead is on the design's
own list of next steps and has not been decided.
