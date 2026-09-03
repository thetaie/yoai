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
RING_R = 36.0
RING_STROKE = 6.0
INK = "#08131F"
ACCENT = "#0072E8"
GROUND = "#EAF3FB"

# (visible arc length, dash offset, opacity) as the design declares them;
# the last segment is the current state and carries the accent color
SEGMENTS = [
    (60, 0, INK, 0.9),
    (34, 68, INK, 0.55),
    (46, 110, INK, 0.3),
    (52, 164, ACCENT, 1.0),
]

# --- output geometry ---
CANVAS = 1024
RENDER = 2048

# Content size on the canvas. The app's original foreground.png used 547, which
# put the mark's farthest pixel at 342px from center -- exactly the 341px radius
# of the conventional two-thirds circular safe zone. That is the strictest mask
# any launcher applies, and sizing to it left the icon reading small.
#
# 620 lets the plus badge cross that circle while the ring itself stays well
# inside it. HarmonyOS launchers mask with a squircle, which is far more
# generous at 45 degrees -- exactly where the badge sits -- so nothing is lost
# there; against a strict circular mask the badge's outer edge would be shaved,
# which costs much less than clipping the ring. Past roughly 740 the ring itself
# breaks out and there is no mask that saves it.
CONTENT = 620


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
        + "\n".join(paths)
        # hands, and the hub that makes them read as a clock rather than a corner
        + f'\n  <path d="M48 48V32M48 48h12" stroke="{INK}" stroke-width="2.5"'
        ' stroke-linecap="round"/>\n'
        f'  <circle cx="48" cy="48" r="3.2" fill="{INK}"/>\n'
        # the plus badge, ground-filled so it punches a hole in the ring
        f'  <circle cx="76" cy="76" r="15" fill="{GROUND}" stroke="{ACCENT}"'
        ' stroke-width="1.5"/>\n'
        f'  <path d="M76 69v14M69 76h14" stroke="{ACCENT}" stroke-width="1.5"'
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
