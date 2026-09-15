/**
 * The icon set: path data from the design, plus the one builder that draws it.
 *
 * Every icon in the app comes through `icon()`. Before this file the app had
 * no icon system at all -- two hand-inlined shapes and, everywhere else,
 * Unicode glyphs (U+2039 for back, U+203A for a row chevron, U+2630 for the
 * drawer, U+25BE for a menu caret). A glyph is not an icon: its ink fills
 * only a fraction of its em box, so `Text('‹').fontSize(24)` draws a hairline
 * about 6vp wide where the layout reserved 24, and raising the font size just
 * gives it a taller box with the same thin ink.
 *
 * Drawn rather than loaded from `$r('app.media.*')`: an icon's stroke is a
 * palette color, and a resource can only carry one value per qualifier, so a
 * base/dark SVG pair follows light and dark but knows nothing about which of
 * the three families is in force -- it would be wrong in four of the six
 * palettes. See design/icons.md.
 *
 * ## Why the commands are scaled here
 *
 * A `Path`'s `commands` are in **px**, while every size in this app -- and
 * every width and height on the component itself -- is in vp. On a 3x screen
 * that means a 24-unit path drawn in a 24vp box fills a third of it, which is
 * exactly how this file first shipped: the icons came out at a third of the
 * space reserved for them, in every screen, at every size.
 *
 * `Shape`'s `viewPort` looks like the fix and is not. Whatever it scales, it
 * does not reconcile px path data with a vp frame, so sizes written into the
 * component had no visible effect at all -- the icons stayed a third-size no
 * matter what number was next to them. So there is no `Shape` here: the grid
 * is converted to px with `vp2px` and baked into the command string, and the
 * only geometry ArkUI is asked for is a plain width and height.
 */

/** The grid the design draws on. Every command below is in these units. */
const GRID: number = 24;

/**
 * Stroke width, in grid units. One weight for every icon.
 *
 * This was 2 for a while, on the strength of per-icon weights measured off an
 * earlier design file -- which is not in this repo and cannot be checked. The
 * canvas settles it at 1.5: that is its default, its stated rule, and what it
 * renders. There is no longer a per-icon exception.
 */
const STROKE: number = 1.5;

/**
 * The three sizes, in vp, matching design/icons.md.
 *
 * These are the design's own numbers and they are correct now that an icon
 * actually fills the box it is given. They were raised twice while the px/vp
 * bug was still in place, which did nothing except reserve more empty space
 * around a third-size mark; both raises are undone here.
 */
export const ICON_TOOLBAR: number = 24;
export const ICON_ROW: number = 20;
export const ICON_INLINE: number = 16;

/** One icon: its name, its subpaths, and its stroke width in grid units. */
interface IconDef {
  name: string;
  commands: string[];
  stroke: number;
}

/**
 * The path data, transcribed from third_party/lucide -- a pinned copy of the
 * icons at one version, which is the source of record.
 *
 * It used to come from an earlier design file instead, on the reasoning that
 * the canvas had been signed off with those shapes. That was wrong twice over:
 * the file is gone, and what the canvas draws IS Lucide -- it loads the library
 * live and only falls back to its own simplified paths when offline, saying so
 * in the corner when it does. The shapes reviewed were Lucide's.
 *
 * Only these six. design/icons.md lists the full set; the rest belong to
 * screens that do not exist yet, and inventing their path data would ship the
 * wrong glyph under the right name.
 *
 * When adding one: take EVERY element from the .svg, not just the `d`
 * attributes. Several icons carry a `<circle>` or `<rect>` that has to be
 * converted -- `settings` below is a gear path plus a circle rewritten as two
 * arcs -- and dropping it loses part of the glyph silently.
 */
const ICONS: IconDef[] = [
  { name: 'menu', commands: ['M4 5h16', 'M4 12h16', 'M4 19h16'], stroke: STROKE },
  { name: 'chevron-left', commands: ['m15 18-6-6 6-6'], stroke: STROKE },
  { name: 'chevron-right', commands: ['m9 18 6-6-6-6'], stroke: STROKE },
  { name: 'chevron-down', commands: ['m6 9 6 6 6-6'], stroke: STROKE },
  { name: 'check', commands: ['M20 6 9 17l-5-5'], stroke: STROKE },
  {
    name: 'settings',
    commands: [
      'M9.671 4.136a2.34 2.34 0 0 1 4.659 0 2.34 2.34 0 0 0 3.319 1.915' +
        ' 2.34 2.34 0 0 1 2.33 4.033 2.34 2.34 0 0 0 0 3.831 2.34 2.34 0 0 1-2.33 4.033' +
        ' 2.34 2.34 0 0 0-3.319 1.915 2.34 2.34 0 0 1-4.659 0 2.34 2.34 0 0 0-3.32-1.915' +
        ' 2.34 2.34 0 0 1-2.33-4.033 2.34 2.34 0 0 0 0-3.831A2.34 2.34 0 0 1 6.35 6.051' +
        ' a2.34 2.34 0 0 0 3.319-1.915',
      'M15 12a3 3 0 1 1-6 0 3 3 0 0 1 6 0'
    ],
    stroke: STROKE
  }
];

/** Empty rather than thrown: a typo should leave a hole, not take the page down. */
function iconFor(name: string): IconDef {
  for (const i of ICONS) {
    if (i.name === name) {
      return i;
    }
  }
  return { name: name, commands: [], stroke: STROKE };
}

/**
 * One path command string with every length multiplied by `k`.
 *
 * A tokenizer rather than a regex because of the one case that makes this
 * more than a search and replace: in an elliptical arc, `a rx ry rot laf sf
 * x y`, the rotation and the two flags are not lengths. Scaling them turns a
 * sweep flag into a number the parser rejects, which is why the argument
 * position is tracked and slots 2 through 4 of every seven pass through
 * verbatim. Numbers run together with their sign -- `-6-6` is two of them --
 * so the number reader stops at the next sign rather than splitting on it.
 */
function scaleCommands(commands: string, k: number): string {
  let out: string = '';
  let cmd: string = '';
  let arg: number = 0;
  let i: number = 0;
  while (i < commands.length) {
    const ch: string = commands.charAt(i);
    if (ch === ' ' || ch === ',') {
      i++;
      continue;
    }
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
      cmd = ch;
      arg = 0;
      out += ch;
      i++;
      continue;
    }
    let j: number = i;
    if (ch === '-' || ch === '+') {
      j++;
    }
    while (j < commands.length) {
      const d: string = commands.charAt(j);
      if ((d >= '0' && d <= '9') || d === '.') {
        j++;
      } else {
        break;
      }
    }
    const raw: string = commands.substring(i, j);
    const slot: number = arg % 7;
    const isArcFlag: boolean = (cmd === 'a' || cmd === 'A') && slot >= 2 && slot <= 4;
    out += ' ';
    out += isArcFlag ? raw : round2(Number.parseFloat(raw) * k);
    arg++;
    i = j;
  }
  return out;
}

/** Two decimals is well under one physical pixel and keeps the string short. */
function round2(v: number): string {
  return (Math.round(v * 100) / 100).toString();
}

/** vp per grid unit, in px: the whole px/vp correction, in one place. */
function unit(size: number): number {
  return vp2px(size) / GRID;
}

/** The subpaths of `name`, scaled to `size` vp. */
function drawCommands(name: string, size: number): string[] {
  const out: string[] = [];
  for (const c of iconFor(name).commands) {
    out.push(scaleCommands(c, unit(size)));
  }
  return out;
}

/**
 * The stroke width, as an explicit px string.
 *
 * Explicit because a bare number would be read as vp and come out three times
 * too heavy next to path data that is in px -- the same unit mismatch that
 * made the icons a third-size, only in the other direction.
 */
function drawStroke(name: string, size: number): string {
  return round2(iconFor(name).stroke * unit(size)) + 'px';
}

/** Draw `name` at `size` vp in `color`. */
@Builder
export function icon(name: string, size: number, color: string) {
  Stack() {
    ForEach(drawCommands(name, size), (c: string) => {
      Path({ commands: c })
        .width(size)
        .height(size)
        .stroke(color)
        .strokeWidth(drawStroke(name, size))
        .strokeLineCap(LineCapStyle.Round)
        .strokeLineJoin(LineJoinStyle.Round)
        .fill(Color.Transparent)
    }, (c: string) => c)
  }
  .width(size)
  .height(size)
  .alignContent(Alignment.TopStart)
}
