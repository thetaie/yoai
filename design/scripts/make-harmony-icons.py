"""Regenerate the HarmonyOS icon set from the logo mark.

    python design/scripts/make-harmony-icons.py

Writes, relative to the repo root:
    examples/harmony/AppScope/resources/base/media/app_icon.png
    examples/harmony/entry/src/main/resources/base/media/foreground.png
    examples/harmony/entry/src/main/resources/base/media/background.png
    examples/harmony/entry/src/main/resources/base/media/startIcon.png

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
# layer; nothing in the mark is filled with it to fake a hole, though the two
# dimmed segments are translucent and are meant to sit on it
GROUND = "#E7EEF6"
# the plus, on the accent disc -- a shade lighter than the tile
PLUS_GLYPH = "#FAFDFF"
FIGURE = "#B7C9DA"

# (visible arc length, dash offset, opacity) as the design declares them;
# the last segment is the current state and carries the accent color
SEGMENTS = [
    (68, 0, INK, 0.9),
    (39, 78, INK, 0.45),
    (53, 125, INK, 0.26),
    (59, 186, ACCENT, 1.0),
]

# The standing figure: head, neck, torso, arms on the hips, legs and feet, each
# a plain shape in a 100 x 200 box of its own, placed by the transform below.
# It is drawn before the arcs and is not clipped to them, so the head and the
# feet pass behind the ring rather than stopping at it.
FIGURE_BOX = "translate(26.5 5) scale(0.43)"
FIGURE_PARTS = (
    '<circle cx="50" cy="32" r="30"/>'
    '<rect x="43" y="56" width="14" height="14" rx="7"/>'
    '<path d="M33 78c0-8 6-13 17-13s17 5 17 13v30c0 8-7 13-17 13s-17-5-17-13z"/>'
    '<path d="M36 80 25 108 46 66" fill="none" stroke-width="13"/>'
    '<path d="M66 82 85 104 70 118" fill="none" stroke-width="13"/>'
    '<path d="M42 116 36 182" fill="none" stroke-width="16"/>'
    '<path d="M58 116 66 182" fill="none" stroke-width="16"/>'
    '<ellipse cx="34" cy="188" rx="10" ry="7"/>'
    '<ellipse cx="68" cy="188" rx="10" ry="7"/>'
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
        # the figure, behind the arcs. Round caps and joins on the whole group:
        # the limbs are strokes, and their ends are what read as shoulders,
        # elbows and ankles.
        f'  <g transform="{FIGURE_BOX}" fill="{FIGURE}" stroke="{FIGURE}"'
        ' stroke-linecap="round" stroke-linejoin="round">'
        f"{FIGURE_PARTS}</g>\n"
        + "\n".join(paths)
        # hands, and the hub that makes them read as a clock rather than a
        # corner. The axis sits above center so they cross the figure's chest
        # rather than its face.
        + f'\n  <path d="M48 44V33M48 44h10" stroke="{INK}" stroke-width="2.6"'
        ' stroke-linecap="round"/>\n'
        f'  <circle cx="48" cy="44" r="2.8" fill="{INK}"/>\n'
        # the plus badge: a solid accent disc laid straight over the ring, with
        # no washer under it -- at this weight the disc reads cleanly against
        # the arc it covers. The glyph runs nearly the full width of the disc.
        f'  <circle cx="74" cy="74" r="15" fill="{ACCENT}"/>\n'
        f'  <path d="M74 61v26M61 74h26" stroke="{PLUS_GLYPH}" stroke-width="2.8"'
        ' stroke-linecap="round"/>\n</svg>\n'
    )


def main():
    root = pathlib.Path(__file__).resolve().parents[2]
    entry = root / "examples/harmony/entry/src/main/resources/base/media"
    appscope = root / "examples/harmony/AppScope/resources/base/media"
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
