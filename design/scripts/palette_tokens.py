"""The palette vocabulary, shared by every generator that reads palettes.json.

Not a generator itself -- it emits nothing. It holds the three things all of
them have to agree about, and it exists because they were previously agreeing by
being copied.

The README's rule used to be "change one, change the other, and run both". That
rule was load-bearing while the rename table lived in two files, and it does not
scale: a third host makes three copies and three chances to drift. A token that
means `bg_page` on the phone and something else on the desktop is precisely the
failure a generated file is supposed to make impossible, so the table that
decides it is now in one place and the generators import it.

What stays per-generator is everything about the OUTPUT -- ArkTS wants a typed
table it can index, CSS wants custom properties a cascade can override, Compose
wants a data class and `Color(0xAARRGGBB)` literals. Those have nothing in
common but the hex values, which is why there are still three generators rather
than one with three back ends.
"""
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "design/palettes.json"

# design name -> app token name. palettes.json names its colors the way the
# design system does; the apps name them the way color.json does. The nine
# tokens the design added in round 10 already use the app's names and need no
# entry here.
RENAME = {
    "surface-page": "bg_page",
    "surface-card": "bg_surface",
    "surface-inverse": "surface_inverse",
    "text-primary": "text_primary",
    "text-tertiary": "text_tertiary",
    "border": "border",
    "accent": "brand",
    "accent-soft": "accent_soft",
    "muted-on-inverse": "muted_on_inverse",
    "accent-on-inverse": "accent_on_inverse",
}

# Order of the fields in every generated file, so they all read in the same
# groups and a diff between two hosts lines up.
FIELDS = [
    "bg_page", "bg_surface", "bg_bubble", "bg_overlay", "surface_inverse",
    "text_primary", "text_secondary", "text_tertiary", "text_disabled",
    "brand", "on_brand", "accent_soft", "muted_on_inverse", "accent_on_inverse",
    "border", "divider", "scrim",
    "danger", "success",
]

# The six palettes are three families of two. An app exposes them as two
# independent controls -- light/dark/system, and the family -- so a picker
# offers three rows, not six.
FAMILIES = ("amber", "arctic", "quiet")

# Fixed rather than sorted, so the picker's order is a decision rather than an
# accident of how the JSON happens to be keyed.
ORDER = ["daylight-amber", "daylight-arctic", "quiet-day",
         "midnight-amber", "midnight-arctic", "quiet-night"]

# Keys in a theme that are not colors.
NON_COLOR = ("label", "labelZh", "mode", "note", "state-ramp")


def family_of(key: str) -> str:
    for f in FAMILIES:
        if f in key:
            return f
    if key in ("quiet-day", "quiet-night"):
        return "quiet"
    raise SystemExit(f"cannot place {key!r} in a family")


def load():
    """Read palettes.json and check the invariants every generator relies on.

    Returns (doc, rows, families): the parsed document, {theme key: {app token:
    hex}}, and {family: {'light': key, 'dark': key}}.

    The checks are here rather than in each generator because they are checks
    about the SOURCE, not about any one output: that every palette carries all
    19 tokens, that none carries a token nothing maps, that each family is one
    light and one dark, and that the set has not changed underneath ORDER. Any
    of them failing means palettes.json was edited without the generators being
    told, which is the moment to stop rather than to emit a file that is quietly
    missing a color.
    """
    doc = json.load(open(SRC, encoding="utf-8"))
    themes = doc["themes"]

    if sorted(ORDER) != sorted(themes):
        raise SystemExit("palette set changed; update ORDER in palette_tokens.py")

    rows = {}
    for key, t in themes.items():
        colors = {}
        for name, value in t.items():
            if name in NON_COLOR:
                continue
            colors[RENAME.get(name, name)] = value
        missing = [f for f in FIELDS if f not in colors]
        if missing:
            raise SystemExit(f"{key}: palettes.json has no {missing}")
        extra = [c for c in colors if c not in FIELDS]
        if extra:
            raise SystemExit(f"{key}: unmapped colors {extra}")
        rows[key] = colors

    families = {}
    for key, t in themes.items():
        families.setdefault(family_of(key), {})[t["mode"]] = key
    for f, members in families.items():
        if set(members) != {"light", "dark"}:
            raise SystemExit(f"family {f} is not a light/dark pair: {members}")

    return doc, rows, families
