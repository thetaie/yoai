/**
 * Settings: two entries and the things that belong to neither.
 *
 * An index rather than one long page. The interface group alone is eleven
 * language rows, five text-size rows and three theme-color rows -- about two
 * and a half screens before any of the model settings, which is what made the
 * single page unusable.
 *
 * Each row carries its current value as a subtitle, so the common reason to
 * open a sub-page -- checking what it is set to -- is answered here.
 *
 * The two rows are written out rather than produced by one @Builder taking a
 * title and a callback: a builder parameterized by a closure is the shape
 * ArkUI's builder parameter passing handles least predictably, the same reason
 * the add form on the models page is written out field by field.
 */
import { common } from '@kit.AbilityKit';
import { router } from '@kit.ArkUI';
import yoai from 'libyoai.so';
import { ModelConfig, modelLabel } from '../model/Provider';
import { ICON_ROW, ICON_TOOLBAR, icon } from '../core/Icons';
import { ModelStore } from '../core/ModelStore';
import { DEFAULT_FAMILY, Palette, familyFor, resolvePalette } from '../core/Palettes';
import { SessionStore } from '../core/SessionStore';

@Entry
@Component
struct Settings {
  /** Subtitle for the models row: what the composer is pointed at right now. */
  @State private activeLabel: string = '';
  @State private modelCount: number = 0;

  /** See the same block in Index -- family is the choice, isDark the outcome. */
  @StorageLink('paletteFamily') @Watch('resolveTheme') paletteFamily: string = DEFAULT_FAMILY;

  @StorageProp('isDark') @Watch('resolveTheme') isDark: boolean = false;

  @State private theme: Palette = resolvePalette(DEFAULT_FAMILY, false);

  private resolveTheme(): void {
    this.theme = resolvePalette(this.paletteFamily, this.isDark);
  }

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  private confirmClearHistory(): void {
    AlertDialog.show({
      title: $r('app.string.settings_clear_history'),
      message: $r('app.string.settings_clear_history_msg'),
      primaryButton: {
        value: $r('app.string.settings_cancel'),
        action: () => {}
      },
      secondaryButton: {
        value: $r('app.string.settings_delete'),
        fontColor: this.theme.danger,
        action: () => {
          SessionStore.clear(this.ctx());
        }
      }
    });
  }

  @Builder
  sectionTitle(text: ResourceStr) {
    Text(text)
      .fontSize(13)
      .fontColor(this.theme.text_tertiary)
      .width('100%')
      .padding({ left: 16, right: 16, top: 18, bottom: 6 })
  }

  async aboutToAppear(): Promise<void> {
    this.resolveTheme();   // @Watch does not fire on construction; see Index
    await this.reloadSummary();
  }

  /**
   * Re-read on every appearance, not just the first: coming back from the
   * models page is exactly when this summary is most likely to be stale.
   */
  onPageShow(): void {
    this.reloadSummary();
  }

  private async reloadSummary(): Promise<void> {
    const list: ModelConfig[] = await ModelStore.list(this.ctx());
    const active: string = await ModelStore.activeId(this.ctx());
    this.modelCount = list.length;
    let label: string = '';
    for (const m of list) {
      if (m.id === active) {
        label = modelLabel(m);
      }
    }
    this.activeLabel = label;
  }

  build() {
    Column() {
      Row({ space: 8 }) {
        Button() {
          icon('chevron-left', ICON_TOOLBAR, this.theme.text_secondary)
        }
        .type(ButtonType.Circle)
        .backgroundColor(Color.Transparent)
        .width(40)
        .height(40)
        .onClick(() => router.back())

        Text($r('app.string.settings_title'))
          .fontSize(18)
          .fontWeight(FontWeight.Medium)
          .fontColor(this.theme.text_primary)
          .layoutWeight(1)
      }
      .width('100%')
      .height(56)
      .padding({ left: 8, right: 16 })

      Scroll() {
        Column() {
          Column() {
            Row({ space: 12 }) {
              Column({ space: 2 }) {
                Text($r('app.string.settings_page_models'))
                  .fontSize(16)
                  .fontColor(this.theme.text_primary)
                  .width('100%')
                Text(this.modelCount === 0
                  ? $r('app.string.settings_models_empty')
                  : this.activeLabel)
                  .fontSize(12)
                  .fontColor(this.theme.text_tertiary)
                  .width('100%')
                  .maxLines(1)
                  .textOverflow({ overflow: TextOverflow.Ellipsis })
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)
              icon('chevron-right', ICON_ROW, this.theme.text_tertiary)
            }
            .width('100%')
            .constraintSize({ minHeight: 56 })
            .padding({ left: 16, right: 16, top: 10, bottom: 10 })
            .onClick(() => router.pushUrl({ url: 'pages/SettingsModels' }))

            Divider().color(this.theme.divider).margin({ left: 16, right: 16 })

            Row({ space: 12 }) {
              Column({ space: 2 }) {
                Text($r('app.string.settings_interface'))
                  .fontSize(16)
                  .fontColor(this.theme.text_primary)
                  .width('100%')
                // The family, in the user's language. Not `this.theme.label`,
                // which is the design's English name for the resolved palette
                // and is not a string resource -- it read "Daylight Arctic" in
                // all eleven languages. Light/dark is left out on purpose: it
                // is legible from the screen the row is drawn on.
                Text(familyFor(this.paletteFamily).label)
                  .fontSize(12)
                  .fontColor(this.theme.text_tertiary)
                  .width('100%')
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)
              icon('chevron-right', ICON_ROW, this.theme.text_tertiary)
            }
            .width('100%')
            .constraintSize({ minHeight: 56 })
            .padding({ left: 16, right: 16, top: 10, bottom: 10 })
            .onClick(() => router.pushUrl({ url: 'pages/SettingsInterface' }))
          }
          .width('100%')
          .margin({ left: 12, right: 12, top: 12 })
          .borderRadius(12)
          .backgroundColor(this.theme.bg_surface)

          this.sectionTitle($r('app.string.settings_core'))
          Column({ space: 12 }) {
            Text(yoai.nativeVersion())
              .fontSize(12)
              .fontColor(this.theme.text_tertiary)
              .width('100%')
            Text($r('app.string.settings_clear_history'))
              .fontSize(15)
              .fontColor(this.theme.danger)
              .width('100%')
              .onClick(() => this.confirmClearHistory())
          }
          .width('100%')
          .padding(16)
          .margin({ left: 12, right: 12, bottom: 24 })
          .borderRadius(12)
          .backgroundColor(this.theme.bg_surface)
        }
        .width('100%')
      }
      .layoutWeight(1)
      .width('100%')
      .align(Alignment.Top)
    }
    .width('100%')
    .height('100%')
    .backgroundColor(this.theme.bg_page)
  }
}
