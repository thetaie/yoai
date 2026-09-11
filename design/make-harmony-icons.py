"""Regenerate the HarmonyOS icon set from the logo mark.

    python design/make-harmony-icons.py

Writes, relative to the repo root:
    harmony/AppScope/resources/base/media/app_icon.png
    harmony/entry/src/main/resources/base/media/foreground.png
    harmony/entry/src/main/resources/base/media/background.png
    harmony/entry/src/main/resources/base/media/startIcon.png

Needs ImageMagick (`magick`) on PATH and Pillow.

This uses the full four-segment mark. The three-segment reduction in
design/logo.md is for thumbnails and other small renderings, not for the icon
assets.

Why the arcs are resolved here instead of being copied from logo.md: the design
expresses the ring as circles cut by stroke-dasharray and stroke-dashoffset.
Browsers render that correctly, but librsvg 2.40 -- the SVG backend inside
ImageMagick 7.1 on Windows -- loses the gaps between segments and overdraws the
accent arc, which came out as a 268-degree blue arc instead of 104. Resolving
each dash into an explicit "M ... A ..." path leaves librsvg nothing to get
wrong. The geometry is identical; only the notation changes.
"""
import math
import pathlib
import subprocess
import sys
import tempfile

from PIL import Image

# --- the mark, on a 96-unit canvas (four-segment variant, per design/logo.md) ---
CX = CY = 48.0
RING_R = 41.0
RING_STROKE = 11.0
INK = "#08131F"
ACCENT = "#0072E8"
# the design's rounded tile, which here becomes the layered icon's background
# layer; nothing inside the mark is filled with it, so the foreground is correct
# over any ground
GROUND = "#E7EEF6"
# the plus, on the accent disc -- a shade lighter than the tile
PLUS_GLYPH = "#FAFDFF"
FIGURE = "#4E6478"
FIGURE_OPACITY = 0.32

# (visible arc length, dash offset, opacity) as the design declares them;
# the last segment is the current state and carries the accent color
SEGMENTS = [
    (68, 0, INK, 0.9),
    (39, 78, INK, 0.5),
    (53, 125, INK, 0.28),
    (59, 186, ACCENT, 1.0),
]

# The standing figure, drawn in a 58 x 200 box of its own and placed inside the
# ring by the transform below. It is a shadow layer: one flat neutral at low
# opacity, so it never competes with the arcs.
FIGURE_BOX = "translate(38.5 15) scale(0.33)"
FIGURE_PATH = (
    "M23.5 0.0L34.1 0.0L38.8 4.7L41.2 9.4L41.2 18.8L38.8 21.2L36.5 28.2L34.1 29.4"
    "L37.6 32.9L42.4 35.3L45.9 35.3L51.8 40.0L52.9 43.5L54.1 64.7L56.5 70.6"
    "L57.6 105.9L55.3 110.6L51.8 112.9L54.1 109.4L52.9 107.1L51.8 108.2L51.8 98.8"
    "L54.1 95.3L54.1 91.8L52.9 90.6L51.8 81.2L49.4 76.5L48.2 62.4L47.1 61.2"
    "L47.1 57.6L45.9 56.5L44.7 57.6L43.5 74.1L42.4 75.3L42.4 81.2L43.5 82.4"
    "L44.7 92.9L47.1 98.8L47.1 120.0L45.9 121.2L45.9 128.2L44.7 129.4L44.7 165.9"
    "L43.5 167.1L43.5 172.9L41.2 180.0L41.2 190.6L44.7 198.8L43.5 200.0L36.5 198.8"
    "L36.5 194.1L35.3 192.9L36.5 174.1L34.1 165.9L34.1 142.4L31.8 135.3L30.6 121.2"
    "L28.2 118.8L25.9 127.1L25.9 135.3L23.5 141.2L23.5 167.1L22.4 168.2L22.4 176.5"
    "L21.2 177.6L22.4 192.9L21.2 194.1L21.2 198.8L18.8 200.0L14.1 200.0L12.9 198.8"
    "L16.5 190.6L16.5 181.2L15.3 180.0L15.3 174.1L12.9 165.9L12.9 130.6L11.8 129.4"
    "L11.8 121.2L10.6 120.0L10.6 100.0L11.8 98.8L11.8 92.9L12.9 91.8L14.1 80.0"
    "L15.3 78.8L14.1 76.5L14.1 64.7L12.9 61.2L11.8 60.0L9.4 62.4L9.4 70.6L8.2 71.8"
    "L7.1 81.2L4.7 85.9L4.7 97.6L5.9 98.8L7.1 108.2L5.9 108.2L4.7 104.7L3.5 105.9"
    "L3.5 109.4L5.9 112.9L2.4 110.6L0.0 105.9L0.0 81.2L1.2 80.0L1.2 70.6L4.7 58.8"
    "L4.7 43.5L9.4 36.5L15.3 35.3L23.5 30.6L23.5 28.2L21.2 25.9L16.5 25.9L15.3 24.7"
    "L15.3 21.2L14.1 20.0L15.3 18.8L14.1 15.3L16.5 12.9L16.5 9.4L17.6 8.2L17.6 2.4"
    "L22.4 1.2Z"
)

# --- output geometry ---
CANVAS = 1024
RENDER = 2048

# Content size on the canvas. The mark's bounding box is exactly the ring's, so
# this number is the ring's outer diameter: at 682 the ring lands precisely on
# the 341px radius of the conventional two-thirds circular safe zone -- the
# strictest mask any launcher applies. That is the largest ring that survives
# every mask intact.
#
# The plus badge sits at 45 degrees and reaches farther from center than the
# ring does, so it crosses that circle -- 380px against 341. HarmonyOS masks
# with a squircle, which is far more generous at 45 degrees, so nothing is lost
# there, and a strict circular mask would shave the badge's outer edge rather
# than cut the ring.
#
# The design instead draws the ring pressing against the edges of its own
# rounded tile, which on a 1024 canvas is 992 of content. That was tried on a
# device and rolled back: the icon sits at the safe zone by choice, and the
# margin the safe zone leaves is visible as a plate around the mark.
CONTENT = 682


def build_svg():
    circumference = 2 * math.pi * RING_R

    def point(phi_deg):
        """Position at phi degrees clockwise from twelve o'clock."""
        phi = math.radians(phi_deg)
        return CX + RING_R * math.sin(phi), CY - RING_R * math.cos(phi)

    paths = []
    for arc_len, offset, color, opacity in SEGMENTS:
        a1 = offset / circumference * 360.0
        a2 = (offset + arc_len) / circumference * 360.0
        x1, y1 = point(a1)
        x2, y2 = point(a2)
        large = 1 if (a2 - a1) > 180 else 0
        op = "" if opacity == 1.0 else f' opacity="{opacity}"'
        paths.append(
            f'  <path d="M {x1:.3f} {y1:.3f} A {RING_R} {RING_R} 0 {large} 1'
            f' {x2:.3f} {y2:.3f}" stroke="{color}"'
            f' stroke-width="{RING_STROKE}" stroke-linecap="round"{op}/>'
        )

    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{RENDER}"'
        f' height="{RENDER}" viewBox="0 0 96 96" fill="none">\n'
        # the figure, behind everything, clipped to the ring's inner circle
        '  <clipPath id="inner"><circle cx="48" cy="48" r="33"/></clipPath>\n'
        f'  <g opacity="{FIGURE_OPACITY}" fill="{FIGURE}" clip-path="url(#inner)">\n'
        f'    <path transform="{FIGURE_BOX}" d="{FIGURE_PATH}"/>\n'
        "  </g>\n"
        + "\n".join(paths)
        # hands, and the hub that makes them read as a clock rather than a
        # corner. The axis sits above center so they cross the figure's chest
        # rather than its face.
        + f'\n  <path d="M48 44V34M48 44h9" stroke="{INK}" stroke-width="2.4"'
        ' stroke-linecap="round"/>\n'
        f'  <circle cx="48" cy="44" r="2.6" fill="{INK}"/>\n'
        # the plus badge: a solid accent disc laid straight over the ring, with
        # no washer under it -- at this weight the disc reads cleanly against
        # the arc it covers
        f'  <circle cx="74" cy="74" r="15" fill="{ACCENT}"/>\n'
        f'  <path d="M74 67v14M67 74h14" stroke="{PLUS_GLYPH}" stroke-width="2.8"'
        ' stroke-linecap="round"/>\n</svg>\n'
    )


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    entry = root / "harmony/entry/src/main/resources/base/media"
    appscope = root / "harmony/AppScope/resources/base/media"
    for d in (entry, appscope):
        if not d.is_dir():
            sys.exit(f"not found: {d}")

    with tempfile.TemporaryDirectory() as tmp:
        svg = pathlib.Path(tmp) / "mark.svg"
        png = pathlib.Path(tmp) / "mark.png"
        svg.write_text(build_svg(), encoding="utf-8")
        subprocess.run(
            ["magick", "-background", "none", str(svg), f"PNG32:{png}"], check=True
        )
        raw = Image.open(png).convert("RGBA")

    mark = raw.crop(raw.split()[3].getbbox()).resize((CONTENT, CONTENT), Image.LANCZOS)
    off = (CANVAS - CONTENT) // 2
    ground = tuple(int(GROUND[i : i + 2], 16) for i in (1, 3, 5)) + (255,)

    # foreground: the mark alone, for the layered icon. Placed with a plain paste
    # (no mask) so the mark's alpha is copied verbatim -- pasting RGBA with itself
    # as the mask would square the alpha and thin out every antialiased edge.
    fg = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    fg.paste(mark, (off, off))

    # app_icon: both layers flattened, for app-management lists
    app = Image.alpha_composite(Image.new("RGBA", (CANVAS, CANVAS), ground), fg)

    fg.save(entry / "foreground.png")
    Image.new("RGBA", (CANVAS, CANVAS), ground).save(entry / "background.png")
    # startIcon carries its own ground rather than compositing over
    # $color:start_window_background: that color is #101315 in the dark theme, and
    # the mark's ink arcs (#08131F) would all but disappear on it.
    app.save(entry / "startIcon.png")
    app.save(appscope / "app_icon.png")

    for p in (
        appscope / "app_icon.png",
        entry / "foreground.png",
        entry / "background.png",
        entry / "startIcon.png",
    ):
        print(f"wrote {p.relative_to(root)}")


if __name__ == "__main__":
    main()
