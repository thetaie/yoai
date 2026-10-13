/**
 * An assistant reply, rendered from the markdown it is written in: an optional
 * thinking block, then the answer.
 *
 * Models write lists, tables and code as markdown, and shown as plain text a
 * reply read as a page of dashes, pipes and fences. Only replies come through
 * here; a user's message is what they typed and is shown as typed. The desktop
 * renders the same way (examples/desktop/src/renderer/src/components/Markdown.tsx),
 * and draws the same lines:
 *
 *   - No images. Loading one fetches a URL the reply chose, which is how a page
 *     is made to report that it was read. The alt text is shown instead.
 *   - A link opens in the system browser, and only an http, https or mailto
 *     one -- a reply is text from a model, and a custom scheme in it must not
 *     launch anything.
 *   - No formulas or charts: a math block or span is shown as its TeX source.
 *
 * Adapted from the mirobody project's HarmonyOS client, with this app's palette
 * in place of color resources and without the render host its formulas and
 * charts went through.
 *
 * The ForEach keys carry a content EXTENT, not just the block's id. ArkUI skips
 * an item whose key is unchanged, so an id that is stable across streaming
 * appends means a growing block never re-renders; the extent churns exactly the
 * one block being appended to and leaves every finished one untouched.
 *
 * Re-parsed on every delta while a reply streams. That is cheap at the length of
 * a reply; what it costs is a marker not yet closed -- `**粗` before its `**`
 * arrives -- showing as written for a moment.
 */
import { common } from '@kit.AbilityKit';
import { InlineSpan, ListEntry, RichBlock, TableRow, parseBlocks } from '../core/Markdown';
import { DEFAULT_FAMILY, Palette, resolvePalette } from '../core/Palettes';

@Component
export struct RichMessage {
  @Prop text: string = '';
  @Prop thinking: string = '';
  @Prop streaming: boolean = false;
  @Prop fontScale: number = 1;

  /** See the same block in Index -- family is the choice, isDark the outcome. */
  @StorageProp('paletteFamily') @Watch('resolveTheme') paletteFamily: string = DEFAULT_FAMILY;
  @StorageProp('isDark') @Watch('resolveTheme') isDark: boolean = false;
  @State private theme: Palette = resolvePalette(DEFAULT_FAMILY, false);

  // Manual fold override: unset until the user taps, then it wins.
  @State private userToggled: boolean = false;
  @State private manualExpand: boolean = false;

  private resolveTheme(): void {
    this.theme = resolvePalette(this.paletteFamily, this.isDark);
  }

  aboutToAppear(): void {
    this.resolveTheme();   // @Watch does not fire on construction
  }

  /** Identity plus how much content the block holds; see the note at the top. */
  private blockKey(b: RichBlock): string {
    let n: number = b.body.length;
    for (const s of b.spans) {
      n += s.text.length;
    }
    for (const e of b.entries) {
      n += e.marker.length;
      for (const s of e.spans) {
        n += s.text.length;
      }
    }
    for (const r of b.rows) {
      for (const cell of r.cells) {
        for (const s of cell) {
          n += s.text.length;
        }
      }
    }
    return b.id + '#' + n;
  }

  /**
   * Open while the model is still thinking and nothing of the answer has
   * arrived, folded after that. A tap always wins over the automatic rule.
   */
  private thinkingExpanded(): boolean {
    if (this.userToggled) {
      return this.manualExpand;
    }
    return this.streaming && this.text.length === 0;
  }

  build() {
    Column({ space: 6 }) {
      // trim(): a model that declines to reason still emits the empty
      // <think></think> pair, whose whitespace body would otherwise draw a
      // handle with nothing behind it.
      if (this.thinking.trim().length > 0) {
        this.thinkingBlock()
      }
      ForEach(parseBlocks(this.text), (b: RichBlock) => {
        this.block(b)
      }, (b: RichBlock) => this.blockKey(b))
    }
    .alignItems(HorizontalAlign.Start)
    // Definite width so the blocks' percentage sizes, and the code blocks'
    // horizontal scrolling, resolve against the bubble.
    .width('100%')
  }

  //----------------------------------------------------------------------------
  // Thinking
  //----------------------------------------------------------------------------

  @Builder
  thinkingBlock() {
    Column({ space: 4 }) {
      Row({ space: 6 }) {
        Text($r('app.string.chat_thinking_label'))
          .fontSize(12)
          .fontColor(this.theme.text_tertiary)
        Text(this.thinkingExpanded() ? '▾' : '▸')
          .fontSize(12)
          .fontColor(this.theme.text_tertiary)
      }
      .onClick(() => {
        this.manualExpand = !this.thinkingExpanded();
        this.userToggled = true;
      })

      if (this.thinkingExpanded()) {
        // Plain, not markdown: it is scratch work, read in passing, and the
        // answer below is what gets the rendering.
        Text(this.thinking)
          .fontSize(14 * this.fontScale)
          .fontColor(this.theme.text_tertiary)
          .width('100%')
      }
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding(10)
    .borderRadius(10)
    .backgroundColor(this.theme.bg_overlay)
  }

  //----------------------------------------------------------------------------
  // Blocks
  //----------------------------------------------------------------------------

  @Builder
  block(b: RichBlock) {
    if (b.kind === 'heading') {
      this.heading(b)
    } else if (b.kind === 'code' || b.kind === 'math') {
      this.codeBlock(b)
    } else if (b.kind === 'quote') {
      this.quote(b)
    } else if (b.kind === 'rule') {
      this.rule()
    } else if (b.kind === 'list') {
      this.list(b)
    } else if (b.kind === 'table') {
      this.table(b)
    } else {
      this.paragraph(b)
    }
  }

  /** Struck or not, since every branch below has to answer the same question. */
  private static line(s: InlineSpan): TextDecorationType {
    return s.strike === true ? TextDecorationType.LineThrough : TextDecorationType.None;
  }

  private openLink(href: string): void {
    if (!href.startsWith('http://') && !href.startsWith('https://') && !href.startsWith('mailto:')) {
      return;
    }
    try {
      (getContext(this) as common.UIAbilityContext).startAbility({
        action: 'ohos.want.action.viewData',
        entities: ['entity.system.browsable'],
        uri: href
      });
    } catch (e) {
      // No handler installed for the scheme. Nothing useful to say about it.
    }
  }

  @Builder
  inline(spans: InlineSpan[], size: number, color: string, weight: FontWeight) {
    Text() {
      ForEach(spans, (s: InlineSpan) => {
        if (s.kind === 'code') {
          Span(s.text)
            .fontFamily('monospace')
            .fontSize(size * 0.92)
            .fontColor(this.theme.text_primary)
            .decoration({ type: RichMessage.line(s) })
            .textBackgroundStyle({ color: this.theme.bg_bubble, radius: 3 })
        } else if (s.kind === 'math') {
          Span(s.text)
            .fontStyle(FontStyle.Italic)
            .fontColor(this.theme.text_secondary)
        } else if (s.kind === 'image') {
          Span('🖼 ' + (s.text.length > 0 ? s.text : (s.href ?? '')))
            .fontSize(size * 0.92)
            .fontColor(this.theme.text_tertiary)
        } else if (s.href !== undefined) {
          Span(s.text)
            .fontColor(this.theme.brand)
            .fontWeight(s.bold === true ? FontWeight.Bold : weight)
            .fontStyle(s.italic === true ? FontStyle.Italic : FontStyle.Normal)
            .decoration({
              type: s.strike === true ? TextDecorationType.LineThrough : TextDecorationType.Underline,
              color: this.theme.brand
            })
            .onClick(() => {
              this.openLink(s.href ?? '');
            })
        } else {
          Span(s.text)
            .fontWeight(s.bold === true ? FontWeight.Bold : weight)
            .fontStyle(s.italic === true ? FontStyle.Italic : FontStyle.Normal)
            .decoration({ type: RichMessage.line(s) })
        }
      }, (s: InlineSpan, i: number) => i + ':' + s.kind + ':' + s.text.length)
    }
    .fontSize(size)
    .fontColor(color)
    .lineHeight(size * 1.5)
    .copyOption(CopyOptions.LocalDevice)
  }

  @Builder
  paragraph(b: RichBlock) {
    this.inline(b.spans, 16 * this.fontScale, this.theme.text_primary, FontWeight.Normal)
  }

  @Builder
  heading(b: RichBlock) {
    Column() {
      this.inline(b.spans,
        (b.level <= 1 ? 21 : b.level === 2 ? 19 : b.level === 3 ? 17.5 : 16.5) * this.fontScale,
        this.theme.text_primary, FontWeight.Bold)
    }
    .margin({ top: b.id.endsWith(':0') ? 0 : 4, bottom: 2 })
  }

  @Builder
  codeBlock(b: RichBlock) {
    Column() {
      if (b.language.length > 0) {
        Text(b.language)
          .fontSize(11 * this.fontScale)
          .fontColor(this.theme.text_tertiary)
          .width('100%')
          .padding({ left: 10, top: 5 })
      }
      Scroll() {
        Text(b.body)
          .fontFamily('monospace')
          .fontSize(13.5 * this.fontScale)
          .fontColor(this.theme.text_primary)
          .lineHeight(20 * this.fontScale)
          .padding({ left: 10, right: 10, top: b.language.length > 0 ? 4 : 8, bottom: 8 })
          .copyOption(CopyOptions.LocalDevice)
      }
      .scrollable(ScrollDirection.Horizontal)
      .scrollBar(BarState.Off)
      .align(Alignment.TopStart)
      .width('100%')
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .backgroundColor(this.theme.bg_surface)
    .border({ width: 1, color: this.theme.border })
    .borderRadius(8)
  }

  /**
   * Thematic break, with a margin of its own: a rule is there to SEPARATE, and
   * at the gap between two paragraphs it would read as one more line.
   */
  @Builder
  rule() {
    Divider()
      .strokeWidth(1)
      .color(this.theme.divider)
      .margin({ top: 6, bottom: 6 })
  }

  @Builder
  quote(b: RichBlock) {
    Row({ space: 8 }) {
      Column()
        .width(3)
        .alignSelf(ItemAlign.Stretch)
        .backgroundColor(this.theme.border)
        .borderRadius(2)
      Column() {
        this.inline(b.spans, 15 * this.fontScale, this.theme.text_secondary, FontWeight.Normal)
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)
    }
    .alignItems(VerticalAlign.Top)
    .width('100%')
  }

  @Builder
  list(b: RichBlock) {
    Column({ space: 3 }) {
      ForEach(b.entries, (e: ListEntry) => {
        Row({ space: 6 }) {
          Text(e.marker)
            .fontSize(15 * this.fontScale)
            // A ticked box earns the accent; everything else is furniture.
            .fontColor(e.checked === true ? this.theme.brand : this.theme.text_secondary)
            .constraintSize({ minWidth: 18 })
          Column() {
            this.inline(e.spans, 16 * this.fontScale, this.theme.text_primary, FontWeight.Normal)
          }
          .layoutWeight(1)
          .alignItems(HorizontalAlign.Start)
        }
        .alignItems(VerticalAlign.Top)
        .padding({ left: e.depth * 14 })
        .width('100%')
      }, (e: ListEntry, idx: number) => {
        let n: number = e.marker.length;
        for (const s of e.spans) {
          n += s.text.length;
        }
        return b.id + '-' + idx + '#' + n;
      })
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
  }

  /**
   * GFM table: equal-weight columns that WRAP rather than a horizontally
   * scrolling grid. On a phone the bubble is narrow and every column matters,
   * and a scroller would hide columns behind a gesture nobody expects.
   */
  @Builder
  table(b: RichBlock) {
    Column() {
      ForEach(b.rows, (row: TableRow, rowIdx: number) => {
        Row() {
          ForEach(row.cells, (cell: InlineSpan[], colIdx: number) => {
            Column() {
              this.inline(cell, (rowIdx === 0 ? 14 : 14.5) * this.fontScale,
                          rowIdx === 0 ? this.theme.text_primary : this.theme.text_secondary,
                          rowIdx === 0 ? FontWeight.Bold : FontWeight.Normal)
            }
            .layoutWeight(1)
            .padding({ left: 8, right: 8, top: 6, bottom: 6 })
            .alignItems(colIdx < b.aligns.length && b.aligns[colIdx] === 'right'
              ? HorizontalAlign.End
              : colIdx < b.aligns.length && b.aligns[colIdx] === 'center'
                ? HorizontalAlign.Center
                : HorizontalAlign.Start)
          }, (cell: InlineSpan[], colIdx: number) => rowIdx + ':' + colIdx + ':' + cell.length)
        }
        .width('100%')
        .alignItems(VerticalAlign.Top)
        .backgroundColor(rowIdx === 0 ? this.theme.bg_overlay : Color.Transparent)
        .border({ width: { bottom: 0.5 }, color: this.theme.divider })
      }, (row: TableRow, rowIdx: number) => {
        let n: number = 0;
        for (const cell of row.cells) {
          for (const s of cell) {
            n += s.text.length;
          }
        }
        return rowIdx + '#' + n;
      })
    }
    .width('100%')
    .margin({ top: 2, bottom: 2 })
    .borderRadius(6)
    .clip(true)
    .border({ width: 0.5, color: this.theme.divider })
  }
}
