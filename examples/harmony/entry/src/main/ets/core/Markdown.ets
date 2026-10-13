/**
 * Markdown -> RichBlock[] — the intermediate representation the chat renderer
 * consumes (components/RichMessage.ets draws one component per block).
 *
 * Taken from the HarmonyOS client of the mirobody project, where it was written
 * and tested against streamed replies, with one change: no fence names a
 * renderer here, so ```echarts and ```svg are code like any other. This app
 * draws no charts, figures or formulas; a `math` block or span is shown as its
 * TeX source.
 *
 * Built for STREAMING text, which drives three design rules:
 *
 *  1. Stable ids. A block's id is "<kind>:<start offset>" — appends never move
 *     earlier text, so every already-started block keeps its id across
 *     re-parses and ArkUI's ForEach diff leaves its component alone. Never use
 *     array indexes: one block growing would re-key everything after it.
 *
 *  2. `closed`. Fenced code and $$ math arrive with their opener long before
 *     their terminator. The flag says which state the block is in; the renderer
 *     decides what that means.
 *
 *  3. Pass order: fences are recognized before anything else on a line-by-line
 *     scan (a `# heading` inside a code block is code, not a heading), and
 *     inline styling never crosses a fence. Inline code spans likewise protect
 *     their contents from bold/italic parsing.
 *
 * Coverage: headings, paragraphs, ordered/unordered lists (with indent nesting),
 * task items, block quotes, thematic breaks, fenced and indented code, $$ and
 * \[ \] math, inline `code` / **bold** / *italic* / ***both*** / ~~strike~~,
 * links, autolinks and bare URLs, images as their alt text, entities, GFM pipe
 * tables.
 */

/**
 * One run of inline content: WHAT it is, plus the styles wrapped around it.
 *
 * `kind` is the content type and the kinds are mutually exclusive — text is drawn,
 * code is drawn in a fixed face with its contents opaque, math and image become
 * something other than text entirely. The styles are INDEPENDENT FLAGS because they
 * genuinely compose: `**~~a~~**` is bold and struck, a link may be any of the three,
 * and the old scheme (one `kind` string carrying `'bolditalic'`) had already been
 * forced to spell out a combination. Adding strike and links to that scheme would
 * have needed a name per subset; flags need one field each and compose for free.
 */
export interface InlineSpan {
  kind: string;      // 'text' | 'code' | 'math' | 'image'
  text: string;      // 'math': the TeX source, delimiters stripped. 'image': the alt text.
  bold?: boolean;
  italic?: boolean;
  strike?: boolean;
  /** Link (or image) target. Present => the run is tappable and drawn as a link. */
  href?: string;
}

/** One table cell's inline content; alignment lives on the block, per column. */
export interface TableRow {
  cells: InlineSpan[][];
}

export interface ListEntry {
  marker: string;        // '•', '1.', or a task box — already display-ready
  depth: number;         // 0-based indent level
  spans: InlineSpan[];
  /** GFM task item: the box is drawn from `checked`, and the text is not struck. */
  task?: boolean;
  checked?: boolean;
}

export interface RichBlock {
  id: string;            // stable across streaming appends: kind + start offset
  kind: string;          // 'paragraph' | 'heading' | 'list' | 'quote' | 'rule' | 'code' | 'math' | 'table'
  closed: boolean;
  level: number;         // heading depth 1..6 (0 otherwise)
  language: string;      // code fence info string ('' otherwise)
  body: string;          // code / math source text
  spans: InlineSpan[];   // heading / paragraph / quote inline content
  entries: ListEntry[];  // list items
  rows: TableRow[];      // table rows; rows[0] is the header
  aligns: string[];      // per column: 'left' | 'center' | 'right'
}

function makeBlock(kind: string, start: number): RichBlock {
  return {
    id: kind + ':' + start,
    kind: kind,
    closed: true,
    level: 0,
    language: '',
    body: '',
    spans: [],
    entries: [],
    rows: [],
    aligns: []
  };
}

//------------------------------------------------------------------------------
// Inline parsing
//------------------------------------------------------------------------------

/**
 * Is the text between two single `$` inline math, or is the `$` a currency sign?
 *
 * Requiring a TeX metacharacter (the earlier rule) rejected the single most common
 * inline formula there is — a bare variable, `$a$` — so models writing
 * "sides $a$ and $b$" showed the dollars literally. The tests, in order:
 *
 *   1. a TeX metacharacter is decisive          -> math
 *   2. a body opening with a digit is a price   -> not math ("$100 and $200" has
 *                                                  body "100 and ")
 *   3. CJK inside means the pair spans prose    -> not math
 *   4. otherwise short and expression-shaped is math, but an interior space is only
 *      allowed alongside an operator, so "five or " stays prose while "x + y" is math
 */
function looksLikeTex(body: string): boolean {
  for (let i = 0; i < body.length; i++) {
    const c = body.charAt(i);
    if (c === '\\' || c === '^' || c === '_' || c === '{' || c === '}') {
      return true;
    }
  }

  const t = body.trim();
  if (t.length === 0 || t.length > 48) {
    return false;
  }
  // A price: "$100", "$1,200.50". Math almost never opens a bare `$…$` with a digit.
  const first = t.charCodeAt(0);
  if (first >= 0x30 && first <= 0x39) {
    return false;
  }

  let hasSpace = false;
  let hasOperator = false;
  for (let i = 0; i < t.length; i++) {
    const c = t.charAt(i);
    const code = t.charCodeAt(i);
    if (code > 0x2e7f) {
      return false;             // CJK (and friends): the $ pair spans prose
    }
    if (c === ' ') {
      hasSpace = true;
    } else if (c === '+' || c === '-' || c === '*' || c === '/' || c === '='
               || c === '<' || c === '>' || c === '(' || c === ')' || c === ','
               || c === '.' || c === '|' || c === '!') {
      hasOperator = true;
    } else if (!((code >= 0x30 && code <= 0x39) ||
                 (code >= 0x41 && code <= 0x5a) ||
                 (code >= 0x61 && code <= 0x7a))) {
      return false;             // anything else is not expression-shaped
    }
  }
  // Several words with no operator is prose that merely sits between two dollars.
  return !hasSpace || hasOperator;
}

/** ASCII punctuation — the exact set CommonMark lets a backslash escape. */
const ESCAPABLE = '!"#$%&\'()*+,-./:;<=>?@[\\]^_`{|}~';

/**
 * The entities a model actually writes. Not the full HTML set (2,000+ names): this
 * is chat prose, where anything past these arrives as a literal ampersand anyway.
 * Numeric forms (&#38; / &#x26;) are handled separately and cover the rest.
 */
const ENTITIES: Map<string, string> = new Map<string, string>([
  ['amp', '&'], ['lt', '<'], ['gt', '>'], ['quot', '"'], ['apos', '\''],
  ['nbsp', ' '], ['copy', '©'], ['reg', '®'], ['trade', '™'],
  ['hellip', '…'], ['mdash', '—'], ['ndash', '–'], ['times', '×'],
  ['divide', '÷'], ['deg', '°'], ['plusmn', '±'], ['micro', 'µ'],
  ['le', '≤'], ['ge', '≥'], ['ne', '≠'], ['rarr', '→'],
  ['larr', '←'], ['bull', '•'], ['middot', '·']
]);

/**
 * Copy a span with one more style wrapped around it.
 *
 * The wrapper NEVER replaces the content kind: code and math are opaque (a fixed
 * face and an image have no bold form to take), but they can still sit inside a link
 * or a struck run, so `href` and the flags apply to every kind alike. An inner href
 * wins over an outer one — the innermost link is the specific one.
 */
function wrap(s: InlineSpan, style: InlineSpan): InlineSpan {
  return {
    kind: s.kind,
    text: s.text,
    bold: s.bold === true || style.bold === true ? true : undefined,
    italic: s.italic === true || style.italic === true ? true : undefined,
    strike: s.strike === true || style.strike === true ? true : undefined,
    href: s.href !== undefined ? s.href : style.href
  };
}

/** Does a run of `mark` starting at `i` close somewhere later in `text`? */
function closerOf(text: string, mark: string, from: number): number {
  return text.indexOf(mark, from);
}

/**
 * The next run of EXACTLY `n` `ch` characters at or after `from`, or -1.
 *
 * "Exactly" is the whole point. Closing on the next occurrence instead made
 * `*a **b** c*` collapse: the search for a single star found the FIRST star of the
 * inner `**`, ended the italic two characters in, and the bold never happened —
 * the text came out right and every style on it was wrong. A run longer than the
 * mark belongs to a different delimiter, so it is stepped over, not matched.
 */
function findRun(text: string, from: number, ch: string, n: number): number {
  let i = from;
  while (i < text.length) {
    if (text.charAt(i) !== ch) {
      i++;
      continue;
    }
    let len = 0;
    while (i + len < text.length && text.charAt(i + len) === ch) {
      len++;
    }
    if (len === n) {
      return i;
    }
    i += len;
  }
  return -1;
}

/**
 * A bare URL, from `i`, or -1. Stops at whitespace and at the trailing punctuation a
 * sentence puts after a link — "see http://e.com." must not swallow the full stop,
 * and a URL inside parentheses must not swallow the closer.
 */
function bareUrlEnd(text: string, i: number): number {
  let end = i;
  while (end < text.length) {
    const c = text.charAt(end);
    if (c === ' ' || c === '\t' || c === '\n' || c === '<' || c === '>') {
      break;
    }
    end++;
  }
  while (end > i) {
    const c = text.charAt(end - 1);
    if (c === '.' || c === ',' || c === ';' || c === ':' || c === '!' || c === '?'
        || c === ')' || c === ']' || c === '}' || c === '"' || c === '\'') {
      end--;
    } else {
      break;
    }
  }
  return end;
}

/** `[label](target)` / `![alt](target)` starting at the bracket; -1 if it is not one. */
interface LinkMatch {
  matched: boolean;
  label: string;
  href: string;
  end: number;      // index just past the closing paren
}

function matchLink(text: string, open: number): LinkMatch {
  const none: LinkMatch = { matched: false, label: '', href: '', end: open };
  // Nested brackets inside the label ("[see [1]](url)") are balanced, not just
  // "first ] wins" — a footnote-ish label is common enough to be worth the counter.
  let depth = 0;
  let close = -1;
  for (let i = open; i < text.length; i++) {
    const c = text.charAt(i);
    if (c === '\\') {
      i++;
    } else if (c === '[') {
      depth++;
    } else if (c === ']') {
      depth--;
      if (depth === 0) {
        close = i;
        break;
      }
    }
  }
  if (close < 0 || close + 1 >= text.length || text.charAt(close + 1) !== '(') {
    return none;
  }
  let paren = 0;
  let rparen = -1;
  for (let i = close + 1; i < text.length; i++) {
    const c = text.charAt(i);
    if (c === '\\') {
      i++;
    } else if (c === '(') {
      paren++;
    } else if (c === ')') {
      paren--;
      if (paren === 0) {
        rparen = i;
        break;
      }
    }
  }
  if (rparen < 0) {
    return none;
  }
  let target = text.substring(close + 2, rparen).trim();
  // `[a](url "title")` — the title is markdown's, not ours to show.
  const space = target.indexOf(' ');
  if (space > 0) {
    target = target.substring(0, space);
  }
  if (target.length === 0) {
    return none;
  }
  return { matched: true, label: text.substring(open + 1, close), href: target, end: rparen + 1 };
}

/**
 * text -> spans. One left-to-right scan, in precedence order:
 *
 *   backslash escape  a `\*` is a literal star and never opens anything
 *   code span         binds tightest; its contents are opaque to everything below
 *   math              \(..\) then $..$ with a TeX-shaped body (see looksLikeTex)
 *   image / link      [label](target), and ![alt](target) before it
 *   autolink, bare URL
 *   emphasis          *** ** * and ___ __ _, then ~~strike~~
 *   entity            &amp; and the numeric forms
 *
 * Everything from "image" down RECURSES into its body, and the wrapper folds into
 * the spans that come back (see wrap). That is what makes `**[a `b` c](url)**` work
 * — every construct nests inside every other one, which is how models actually
 * write.
 *
 * Unterminated markers fall back to literal text, so a streaming tail like
 * "some **bo" renders as written instead of eating the rest of the message.
 */
export function parseInline(text: string): InlineSpan[] {
  const spans: InlineSpan[] = [];
  let plain = '';
  let i = 0;

  const flushPlain = (): void => {
    if (plain.length > 0) {
      spans.push({ kind: 'text', text: plain });
      plain = '';
    }
  };

  while (i < text.length) {
    const c = text.charAt(i);

    // \( ... \) — inline math. BEFORE the general escape below, which would
    // otherwise turn the opener into a literal paren. A `\(` with no `\)` still
    // reaches the escape and comes out as "(", which is what CommonMark says.
    if (c === '\\' && i + 1 < text.length && text.charAt(i + 1) === '(') {
      const end = text.indexOf('\\)', i + 2);
      if (end > i + 2) {
        flushPlain();
        spans.push({ kind: 'math', text: text.substring(i + 2, end).trim() });
        i = end + 2;
        continue;
      }
    }

    // Backslash escape: \* is a literal star that opens nothing. First, because a
    // marker it protects must never be seen by the branches below. Only ASCII
    // punctuation is escapable — "C:\path" and "\n" keep their backslash.
    if (c === '\\' && i + 1 < text.length && ESCAPABLE.indexOf(text.charAt(i + 1)) >= 0) {
      plain += text.charAt(i + 1);
      i += 2;
      continue;
    }

    // Code span. The opening run's LENGTH is the closer: ``a ` b`` holds a backtick,
    // and matching on a single one turned that into two broken spans and a stray
    // tick. CommonMark also strips one leading and trailing space, which is what
    // lets `` ` `` be a code span holding just a backtick.
    if (c === '`') {
      let ticks = 1;
      while (i + ticks < text.length && text.charAt(i + ticks) === '`') {
        ticks++;
      }
      const mark = text.substring(i, i + ticks);
      const end = closerOf(text, mark, i + ticks);
      if (end > 0) {
        let body = text.substring(i + ticks, end);
        if (body.length > 1 && body.startsWith(' ') && body.endsWith(' ')) {
          body = body.substring(1, body.length - 1);
        }
        flushPlain();
        spans.push({ kind: 'code', text: body });
        i = end + ticks;
        continue;
      }
      // No closing run (yet) — literal.
      plain += mark;
      i += ticks;
      continue;
    }

    // $ ... $ — inline math only when the body is TeX-shaped (see looksLikeTex),
    // so prices ("$100 and $200") stay literal.
    if (c === '$' && (i + 1 >= text.length || text.charAt(i + 1) !== '$')) {
      const end = text.indexOf('$', i + 1);
      if (end > i + 1) {
        const body = text.substring(i + 1, end);
        if (looksLikeTex(body)) {
          flushPlain();
          spans.push({ kind: 'math', text: body.trim() });
          i = end + 1;
          continue;
        }
      }
      plain += c;
      i++;
      continue;
    }

    // Image, before the link branch it otherwise looks like. The alt text is a LEAF:
    // it stands in for a picture, so styling inside it has nothing to style.
    if (c === '!' && i + 1 < text.length && text.charAt(i + 1) === '[') {
      const m = matchLink(text, i + 1);
      if (m.matched) {
        flushPlain();
        spans.push({ kind: 'image', text: m.label, href: m.href });
        i = m.end;
        continue;
      }
    }

    if (c === '[') {
      const m = matchLink(text, i);
      if (m.matched) {
        flushPlain();
        // An empty label would render as nothing tappable; show the target instead.
        const body = m.label.length > 0 ? m.label : m.href;
        for (const s of parseInline(body)) {
          spans.push(wrap(s, { kind: 'text', text: '', href: m.href }));
        }
        i = m.end;
        continue;
      }
    }

    // <http://e.com> — an autolink is its own label.
    if (c === '<') {
      const gt = text.indexOf('>', i + 1);
      if (gt > i + 1) {
        const inner = text.substring(i + 1, gt);
        if (inner.startsWith('http://') || inner.startsWith('https://')
            || inner.startsWith('mailto:')) {
          flushPlain();
          spans.push({ kind: 'text', text: inner, href: inner });
          i = gt + 1;
          continue;
        }
      }
    }

    // Bare URL. Only at a word boundary, so a query string inside a longer token
    // ("...&url=https://x") does not start a second link mid-word.
    if ((c === 'h' || c === 'w') && (i === 0 || ' \t\n([<"\'' .indexOf(text.charAt(i - 1)) >= 0)) {
      const rest = text.substring(i);
      const isUrl = rest.startsWith('http://') || rest.startsWith('https://');
      const isWww = rest.startsWith('www.');
      if (isUrl || isWww) {
        const end = bareUrlEnd(text, i);
        const raw = text.substring(i, end);
        if (raw.length > (isWww ? 4 : 8)) {
          flushPlain();
          spans.push({ kind: 'text', text: raw, href: isWww ? 'https://' + raw : raw });
          i = end;
          continue;
        }
      }
    }

    // ~~strike~~ (GFM). Exactly two tildes; a single one is prose.
    if (c === '~' && i + 1 < text.length && text.charAt(i + 1) === '~') {
      const end = closerOf(text, '~~', i + 2);
      if (end > i + 2) {
        flushPlain();
        for (const s of parseInline(text.substring(i + 2, end))) {
          spans.push(wrap(s, { kind: 'text', text: '', strike: true }));
        }
        i = end + 2;
        continue;
      }
    }

    // Emphasis: *** ** * and ___ __ _.
    //
    // Underscore is INTRAWORD-BLIND, matching CommonMark and for the same reason:
    // snake_case_names are everywhere in this app's content (column names, model
    // ids, file names) and every one of them would otherwise turn italic. A star
    // has no such problem, so `sn*ake*case` still emphasizes.
    if (c === '*' || c === '_') {
      const wordBefore = i > 0 && isWordChar(text.charAt(i - 1));
      if (c === '*' || !wordBefore) {
        let n = 1;
        while (i + n < text.length && text.charAt(i + n) === c && n < 3) {
          n++;
        }
        const mark = text.substring(i, i + n);
        let end = findRun(text, i + n, c, n);
        if (c === '_') {
          // Skip closers that sit inside a word, for the same reason as the opener.
          while (end >= 0 && end + n < text.length && isWordChar(text.charAt(end + n))) {
            end = findRun(text, end + n, c, n);
          }
        }
        if (end > i + n) {
          flushPlain();
          const style: InlineSpan = {
            kind: 'text',
            text: '',
            bold: n >= 2 ? true : undefined,
            italic: n === 1 || n === 3 ? true : undefined
          };
          // Emphasis WRAPS inline content, it is not a leaf. Storing the body raw left
          // every other marker inside it unparsed, so "**sodium ($Na^+$)**" showed its
          // dollars literally: the math arm above never saw a body that `**` had already
          // swallowed. Recursing costs nothing (the body is strictly shorter, so this
          // terminates) and is the only way math, code and links work inside bold —
          // which is where a model puts them most of the time.
          for (const s of parseInline(text.substring(i + n, end))) {
            spans.push(wrap(s, style));
          }
          i = end + n;
          continue;
        }
        plain += mark;
        i += n;
        continue;
      }
    }

    // &amp; / &#38; / &#x26;
    if (c === '&') {
      const semi = text.indexOf(';', i + 1);
      if (semi > i + 1 && semi - i <= 10) {
        const name = text.substring(i + 1, semi);
        const named = ENTITIES.get(name);
        if (named !== undefined) {
          plain += named;
          i = semi + 1;
          continue;
        }
        const decoded = decodeNumericEntity(name);
        if (decoded.length > 0) {
          plain += decoded;
          i = semi + 1;
          continue;
        }
      }
    }

    plain += c;
    i++;
  }
  flushPlain();
  return spans;
}

/** Letter, digit or underscore — what "inside a word" means for `_` emphasis. */
function isWordChar(c: string): boolean {
  const code = c.charCodeAt(0);
  return (code >= 0x30 && code <= 0x39) || (code >= 0x41 && code <= 0x5a)
      || (code >= 0x61 && code <= 0x7a) || c === '_' || code > 0x7f;
}

/** "#38" / "#x26" -> the character, or '' when it is not a numeric entity. */
function decodeNumericEntity(name: string): string {
  if (!name.startsWith('#') || name.length < 2) {
    return '';
  }
  const hex = name.charAt(1) === 'x' || name.charAt(1) === 'X';
  const digits = name.substring(hex ? 2 : 1);
  if (digits.length === 0) {
    return '';
  }
  const n = Number.parseInt(digits, hex ? 16 : 10);
  if (Number.isNaN(n) || n <= 0 || n > 0x10ffff) {
    return '';
  }
  return String.fromCodePoint(n);
}

//------------------------------------------------------------------------------
// Block parsing
//------------------------------------------------------------------------------

interface Line {
  text: string;    // without the terminator
  start: number;   // char offset of the line's first char in the input
}

function splitLines(text: string): Line[] {
  const lines: Line[] = [];
  let start = 0;
  for (let i = 0; i <= text.length; i++) {
    if (i === text.length || text.charAt(i) === '\n') {
      lines.push({ text: text.substring(start, i), start: start });
      start = i + 1;
    }
  }
  return lines;
}

/** An opening/closing code fence: which character it is made of, and how long. */
interface Fence {
  char: string;
  length: number;
}

/** The leading run of ``` or ~~~ on a trimmed line, or length 0 if there is none. */
function fenceRun(trimmed: string): Fence {
  const c = trimmed.charAt(0);
  if (c !== '`' && c !== '~') {
    return { char: '', length: 0 };
  }
  let n = 0;
  while (n < trimmed.length && trimmed.charAt(n) === c) {
    n++;
  }
  return n >= 3 ? { char: c, length: n } : { char: '', length: 0 };
}

/** A run of `=` alone on the line — a setext H1 underline. */
function isSetextH1(trimmed: string): boolean {
  if (trimmed.length === 0) {
    return false;
  }
  for (let i = 0; i < trimmed.length; i++) {
    if (trimmed.charAt(i) !== '=') {
      return false;
    }
  }
  return true;
}

/** Drop up to `n` columns of leading whitespace, counting a tab as 2 like indentOf. */
function stripIndent(s: string, n: number): string {
  let col = 0;
  let i = 0;
  while (i < s.length && col < n) {
    const c = s.charAt(i);
    if (c === ' ') {
      col += 1;
    } else if (c === '\t') {
      col += 2;
    } else {
      break;
    }
    i++;
  }
  return s.substring(i);
}

/** Leading-space count with tabs as 2. */
function indentOf(s: string): number {
  let n = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s.charAt(i);
    if (c === ' ') {
      n += 1;
    } else if (c === '\t') {
      n += 2;
    } else {
      break;
    }
  }
  return n;
}

interface ListMatch {
  matched: boolean;
  ordered: boolean;
  marker: string;
  rest: string;
  task?: boolean;
  checked?: boolean;
}

/** "- x" / "* x" / "+ x" / "12. x" / "12) x" (after indent removal). */
/**
 * A thematic break: three or more of `-`, `_` or `*`, alone on the line, with
 * spaces allowed between them.
 *
 * MUST be tested before the list matcher, which `- - -` and `* * *` would otherwise
 * satisfy — both open with a marker character followed by a space, so a horizontal
 * rule written that way became a three-item bullet list of empty items.
 */
function isThematicBreak(s: string): boolean {
  const c = s.charAt(0);
  if (c !== '-' && c !== '_' && c !== '*') {
    return false;
  }
  let n = 0;
  for (let i = 0; i < s.length; i++) {
    const ch = s.charAt(i);
    if (ch === c) {
      n++;
    } else if (ch !== ' ' && ch !== '\t') {
      return false;
    }
  }
  return n >= 3;
}

function matchListItem(s: string): ListMatch {
  const none: ListMatch = { matched: false, ordered: false, marker: '', rest: '' };
  if (s.length < 2) {
    return none;
  }
  const c = s.charAt(0);
  if ((c === '-' || c === '*' || c === '+') && s.charAt(1) === ' ') {
    const rest = s.substring(2);
    // GFM task item. The box replaces the bullet rather than joining it: two glyphs
    // of furniture in front of every line is what a checklist is trying to avoid.
    const box = rest.length >= 3 && rest.charAt(0) === '[' && rest.charAt(2) === ']'
                && (rest.charAt(3) === ' ' || rest.length === 3);
    if (box) {
      const mark = rest.charAt(1);
      if (mark === ' ' || mark === 'x' || mark === 'X') {
        const done = mark !== ' ';
        return {
          matched: true, ordered: false, marker: done ? '☑' : '☐',
          rest: rest.substring(rest.length === 3 ? 3 : 4),
          task: true, checked: done
        };
      }
    }
    return { matched: true, ordered: false, marker: '•', rest: rest };
  }
  let d = 0;
  while (d < s.length && d < 3 && s.charAt(d) >= '0' && s.charAt(d) <= '9') {
    d++;
  }
  if (d > 0 && d + 1 < s.length &&
      (s.charAt(d) === '.' || s.charAt(d) === ')') && s.charAt(d + 1) === ' ') {
    return { matched: true, ordered: true, marker: s.substring(0, d) + '.', rest: s.substring(d + 2) };
  }
  return none;
}

/**
 * Split one table line into raw cells on UNESCAPED pipes, dropping the optional
 * leading/trailing delimiters GFM allows. `\|` is a literal pipe in a cell.
 */
function splitCells(line: string): string[] {
  const out: string[] = [];
  let cur = '';
  for (let i = 0; i < line.length; i++) {
    const c = line.charAt(i);
    if (c === '\\' && i + 1 < line.length && line.charAt(i + 1) === '|') {
      cur += '|';
      i++;
    } else if (c === '|') {
      out.push(cur);
      cur = '';
    } else {
      cur += c;
    }
  }
  out.push(cur);
  // A row written as |a|b| yields an empty first and last cell; those are the
  // optional outer delimiters, not real columns.
  if (out.length > 0 && out[0].trim().length === 0) {
    out.shift();
  }
  if (out.length > 0 && out[out.length - 1].trim().length === 0) {
    out.pop();
  }
  return out;
}

/**
 * Alignments if `line` is a GFM delimiter row (`|---|:--:|---:|`), else [].
 *
 * The delimiter row is what MAKES a table: a header-looking line on its own is
 * just a paragraph that happens to contain pipes, which also means a table being
 * streamed only becomes one once its second line lands.
 */
function tableAligns(line: string): string[] {
  const cells = splitCells(line);
  if (cells.length === 0) {
    return [];
  }
  const out: string[] = [];
  for (const raw of cells) {
    const c = raw.trim();
    const left = c.startsWith(':');
    const right = c.endsWith(':');
    const bar = c.substring(left ? 1 : 0, c.length - (right ? 1 : 0));
    if (bar.length === 0) {
      return [];
    }
    for (let i = 0; i < bar.length; i++) {
      if (bar.charAt(i) !== '-') {
        return [];
      }
    }
    out.push(left && right ? 'center' : right ? 'right' : 'left');
  }
  return out;
}

function makeRow(line: string, columns: number): TableRow {
  const raw = splitCells(line);
  const cells: InlineSpan[][] = [];
  for (let i = 0; i < columns; i++) {
    // Pad short rows and ignore extra cells, so a ragged table still renders as a
    // grid instead of collapsing.
    cells.push(parseInline(i < raw.length ? raw[i].trim() : ''));
  }
  return { cells: cells };
}

export function parseBlocks(text: string): RichBlock[] {
  const blocks: RichBlock[] = [];
  const lines = splitLines(text);
  let i = 0;

  // Paragraph accumulator — consecutive plain lines joined with '\n'.
  let para: string[] = [];
  let paraStart = 0;
  const flushPara = (): void => {
    if (para.length > 0) {
      const b = makeBlock('paragraph', paraStart);
      b.spans = parseInline(para.join('\n'));
      blocks.push(b);
      para = [];
    }
  };
  // Setext turns the paragraph ALREADY accumulated into a heading, so it cannot go
  // through flushPara — by the time the underline is read, the title is pending.
  const flushParaAsHeading = (): void => {
    const b = makeBlock('heading', paraStart);
    b.level = 1;
    b.spans = parseInline(para.join('\n'));
    blocks.push(b);
    para = [];
  };

  while (i < lines.length) {
    const line = lines[i];
    const t = line.text;
    const trimmed = t.trim();

    // --- fenced code -------------------------------------------------------
    //
    // The opening run's LENGTH and CHARACTER are both part of the fence. Matching
    // "starts with ```" and closing on exactly "```" meant a ````-fenced block —
    // which is what a model writes when the code itself contains a triple backtick —
    // took its 4th tick as the language and never found a closer, swallowing the
    // rest of the message into an open code block.
    {
      const fence = fenceRun(trimmed);
      if (fence.length >= 3) {
        flushPara();
        const info = trimmed.substring(fence.length).trim();
        // Every fence is code, whatever its info string names: a fence whose
        // body we cannot draw still has to show its body.
        const b = makeBlock('code', line.start);
        b.language = info;
        const body: string[] = [];
        let closed = false;
        i++;
        while (i < lines.length) {
          const cl = fenceRun(lines[i].text.trim());
          // A closer is the same character, at least as long, and carries no info.
          if (cl.char === fence.char && cl.length >= fence.length
              && lines[i].text.trim().length === cl.length) {
            closed = true;
            i++;
            break;
          }
          body.push(lines[i].text);
          i++;
        }
        b.body = body.join('\n');
        b.closed = closed;
        blocks.push(b);
        continue;
      }
    }

    // --- display math: $$ ... $$ and \[ ... \] --------------------------------
    {
      const isDollar  = trimmed === '$$' || (trimmed.startsWith('$$') && trimmed.length > 2 && trimmed.endsWith('$$'));
      const isBracket = trimmed === '\\[' || (trimmed.startsWith('\\[') && trimmed.endsWith('\\]') && trimmed.length > 2);
      if (isDollar || isBracket) {
        flushPara();
        const b = makeBlock('math', line.start);
        const open = isDollar ? '$$' : '\\[';
        const close = isDollar ? '$$' : '\\]';
        if (trimmed.length > open.length && trimmed.endsWith(close)) {
          // One-liner: $$ e = mc^2 $$  /  \[ e = mc^2 \]
          b.body = trimmed.substring(open.length, trimmed.length - close.length).trim();
          b.closed = true;
          i++;
        } else {
          const body: string[] = [];
          let closed = false;
          i++;
          while (i < lines.length) {
            if (lines[i].text.trim() === close) {
              closed = true;
              i++;
              break;
            }
            body.push(lines[i].text);
            i++;
          }
          b.body = body.join('\n');
          b.closed = closed;
        }
        blocks.push(b);
        continue;
      }
    }

    // --- setext H1 ------------------------------------------------------------
    // `===` under a paragraph, and only there: alone it is meaningless text with no
    // competing reading, so honouring it costs nothing. Its `---` sibling is NOT
    // supported and stays a thematic break — see the note on that branch.
    if (para.length > 0 && isSetextH1(trimmed)) {
      flushParaAsHeading();
      i++;
      continue;
    }

    // --- heading --------------------------------------------------------------
    if (trimmed.startsWith('#')) {
      let level = 0;
      while (level < trimmed.length && level < 6 && trimmed.charAt(level) === '#') {
        level++;
      }
      if (level < trimmed.length && trimmed.charAt(level) === ' ') {
        flushPara();
        const b = makeBlock('heading', line.start);
        b.level = level;
        b.spans = parseInline(trimmed.substring(level + 1).trim());
        blocks.push(b);
        i++;
        continue;
      }
    }

    // --- thematic break ---------------------------------------------------------
    // Before the list branch (see isThematicBreak), and after the table branch has
    // had its say further down is not an option either — but it does not need one:
    // a delimiter row is consumed by the header line that looked ahead to it, and
    // one written bare (`--- | ---`) holds pipes, which this rejects.
    //
    // NOT setext: CommonMark reads `---` directly under a paragraph as an H2
    // underline, and honouring that here would be a worse trade than missing it.
    // A model ending a section and starting the next with a `---` separator
    // routinely omits the blank line, and turning that paragraph into a giant
    // heading is a louder failure than a rule rendered where a heading was meant.
    // Models reach for `##` when they want a heading.
    if (isThematicBreak(trimmed)) {
      flushPara();
      blocks.push(makeBlock('rule', line.start));
      i++;
      continue;
    }

    // --- block quote (consecutive > lines merge) --------------------------------
    if (trimmed.startsWith('>')) {
      flushPara();
      const b = makeBlock('quote', line.start);
      const body: string[] = [];
      while (i < lines.length) {
        const q = lines[i].text.trim();
        if (!q.startsWith('>')) {
          break;
        }
        // Strip EVERY leading marker, not one: `>> inner` used to keep a literal
        // ">" in the text. The quote renders flat either way (its body is inline
        // content, not blocks), so a nested quote reads as part of the same quote —
        // which beats showing the reader a stray angle bracket.
        let rest = q;
        while (rest.startsWith('>')) {
          rest = rest.substring(1);
          if (rest.startsWith(' ')) {
            rest = rest.substring(1);
          }
        }
        body.push(rest);
        i++;
      }
      b.spans = parseInline(body.join('\n'));
      blocks.push(b);
      continue;
    }

    // --- list (consecutive items merge; indent -> depth) -------------------------
    {
      const indent = indentOf(t);
      const m = matchListItem(t.substring(indent));
      if (m.matched) {
        flushPara();
        const b = makeBlock('list', line.start);
        // Raw item text, so a continuation line can be appended BEFORE it is parsed
        // — parsing per line would cut a `**bold` that wraps across the two.
        const raw: string[] = [];
        while (i < lines.length) {
          const it = lines[i].text;
          const ind = indentOf(it);
          const im = matchListItem(it.substring(ind));
          if (im.matched) {
            b.entries.push({
              marker: im.marker,
              depth: Math.floor(ind / 2),
              spans: [],
              task: im.task,
              checked: im.checked
            });
            raw.push(im.rest);
            i++;
            continue;
          }
          // Continuation of the item above: indented, non-blank, and not the start of
          // some other block. Two spaces is the bar, not zero — CommonMark's LAZY
          // continuation would also absorb an unindented line, and after a list an
          // unindented line is far more often the next paragraph than more of the
          // last bullet.
          if (raw.length > 0 && ind >= 2 && it.trim().length > 0
              && fenceRun(it.trim()).length === 0 && !it.trim().startsWith('>')
              && !it.trim().startsWith('#')) {
            raw[raw.length - 1] += '\n' + it.trim();
            i++;
            continue;
          }
          break;
        }
        for (let k = 0; k < b.entries.length; k++) {
          b.entries[k].spans = parseInline(raw[k]);
        }
        blocks.push(b);
        continue;
      }
    }

    // --- indented code block (4 spaces) -----------------------------------------
    // Only where CommonMark allows one: not interrupting a paragraph, and after the
    // list branch above has taken its continuations — inside a list, four spaces is
    // item content, not code. Trailing blank lines are trimmed back off so a gap
    // before the next paragraph does not become empty lines inside the block.
    if (para.length === 0 && indentOf(t) >= 4 && trimmed.length > 0) {
      const body: string[] = [];
      while (i < lines.length) {
        const it = lines[i].text;
        if (it.trim().length === 0) {
          body.push('');
          i++;
          continue;
        }
        if (indentOf(it) < 4) {
          break;
        }
        body.push(stripIndent(it, 4));
        i++;
      }
      while (body.length > 0 && body[body.length - 1].length === 0) {
        body.pop();
        i--;
      }
      const b = makeBlock('code', line.start);
      b.body = body.join('\n');
      blocks.push(b);
      continue;
    }

    // --- GFM pipe table -----------------------------------------------------
    // Needs the NEXT line to be a delimiter row; that is what distinguishes a
    // table from a paragraph containing pipes.
    if (trimmed.indexOf('|') >= 0 && i + 1 < lines.length) {
      const aligns = tableAligns(lines[i + 1].text.trim());
      if (aligns.length > 0) {
        flushPara();
        const b = makeBlock('table', line.start);
        b.aligns = aligns;
        b.rows.push(makeRow(t, aligns.length));
        i += 2;                                  // header + delimiter consumed
        while (i < lines.length) {
          const rowText = lines[i].text.trim();
          if (rowText.length === 0 || rowText.indexOf('|') < 0) {
            break;                               // blank line or non-row ends it
          }
          b.rows.push(makeRow(lines[i].text, aligns.length));
          i++;
        }
        blocks.push(b);
        continue;
      }
    }

    // --- blank line = paragraph break -------------------------------------------
    if (trimmed.length === 0) {
      flushPara();
      i++;
      continue;
    }

    // --- plain paragraph line -----------------------------------------------------
    if (para.length === 0) {
      paraStart = line.start;
    }
    para.push(t);
    i++;
  }

  flushPara();
  return blocks;
}
