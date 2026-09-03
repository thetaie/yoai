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
 * Default stroke width, in grid units.
 *
 * 2, not the 1.5 design/icons.md proposes. 1.5 was never what the design
 * actually drew -- every icon in the design file is between 1.75 and 2.6, and
 * icons.md itself records 1.5 as unconfirmed and left as a live control.
 */
const STROKE: number = 2;

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
 * The path data, taken from the design file rather than from Lucide.
 *
 * Where the two differ the design wins: its `check` is `m5 13 4 4L19 7` at
 * stroke 2.6, which sits lower and heavier than Lucide's `M20 6 9 17l-5-5` at
 * 1.5, and the heavier one is what the screens were signed off on.
 *
 * Only these six. design/icons.md lists 24 for the design as a whole; the
 * rest belong to screens that do not exist yet, and inventing their path data
 * would ship the wrong glyph under the right name.
 */
const ICONS: IconDef[] = [
  { name: 'menu', commands: ['M4 6h16', 'M4 12h16', 'M4 18h16'], stroke: STROKE },
  { name: 'chevron-left', commands: ['m15 18-6-6 6-6'], stroke: STROKE },
  { name: 'chevron-right', commands: ['m9 18 6-6-6-6'], stroke: STROKE },
  { name: 'chevron-down', commands: ['m6 9 6 6 6-6'], stroke: STROKE },
  // 2.6 is the design's own weight for the tick; it is the one icon that has
  // to read as a state at a glance rather than as decoration.
  { name: 'check', commands: ['m5 13 4 4L19 7'], stroke: 2.6 },
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
