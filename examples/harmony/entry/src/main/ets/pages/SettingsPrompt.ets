/**
 * The system prompt: shown, edited, and put back to the core's default.
 *
 * The desktop's settings section of the same name, on a page of its own. The
 * box shows the core's default until the user writes over it, and saves when it
 * loses focus and when the page is left -- not per keystroke: each save is
 * also a warm-up of the on-device model's prompt, which is seconds of work.
 */
import { common } from '@kit.AbilityKit';
import { router } from '@kit.ArkUI';
import { ChatSettings, PromptSetting } from '../core/ChatSettings';
import { ICON_TOOLBAR, icon } from '../core/Icons';
import { ModelStore } from '../core/ModelStore';
import { DEFAULT_FAMILY, Palette, resolvePalette } from '../core/Palettes';

@Entry
@Component
struct SettingsPrompt {
  @State private text: string = '';
  @State private custom: boolean = false;
  /** What is stored, to tell an edit from a box left as it was. */
  private saved: string = '';
  private loaded: boolean = false;

  @StorageLink('paletteFamily') @Watch('resolveTheme') paletteFamily: string = DEFAULT_FAMILY;
  @StorageProp('isDark') @Watch('resolveTheme') isDark: boolean = false;
  @State private theme: Palette = resolvePalette(DEFAULT_FAMILY, false);

  private resolveTheme(): void {
    this.theme = resolvePalette(this.paletteFamily, this.isDark);
  }

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  async aboutToAppear(): Promise<void> {
    this.resolveTheme();   // @Watch does not fire on construction; see Index
    this.show(await ChatSettings.get(this.ctx()));
    this.loaded = true;
  }

  aboutToDisappear(): void {
    this.save();
  }

  private show(p: PromptSetting): void {
    this.text = p.text;
    this.saved = p.text;
    this.custom = p.custom;
  }

  /** Store the box, unless it is as it was: nothing to store and nothing to warm again. */
  private async save(): Promise<void> {
    if (!this.loaded || this.text === this.saved) {
      return;
    }
    this.show(await ChatSettings.set(this.ctx(), this.text));
    ModelStore.warmActive(this.ctx());   // the front the next turn shares has changed
  }

  private async reset(): Promise<void> {
    this.show(await ChatSettings.set(this.ctx(), null));
    ModelStore.warmActive(this.ctx());
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

        Text($r('app.string.settings_page_prompt'))
          .fontSize(18)
          .fontWeight(FontWeight.Medium)
          .fontColor(this.theme.text_primary)
          .layoutWeight(1)
      }
      .width('100%')
      .height(56)
      .padding({ left: 8, right: 16 })

      Scroll() {
        Column({ space: 12 }) {
          Text($r('app.string.settings_prompt_intro'))
            .fontSize(12)
            .fontColor(this.theme.text_tertiary)
            .width('100%')

          TextArea({ text: this.text, placeholder: $r('app.string.settings_prompt_hint') })
            .width('100%')
            .constraintSize({ minHeight: 200 })
            .fontSize(14)
            .backgroundColor(this.theme.bg_surface)
            .fontColor(this.theme.text_primary)
            .onChange((v: string) => { this.text = v; })
            .onBlur(() => { this.save(); })

          Row({ space: 12 }) {
            Text(this.custom
              ? $r('app.string.settings_prompt_custom')
              : $r('app.string.settings_prompt_default'))
              .fontSize(12)
              .fontColor(this.theme.text_tertiary)
              .layoutWeight(1)
            if (this.custom) {
              Button($r('app.string.settings_prompt_reset'))
                .height(32)
                .fontSize(13)
                .backgroundColor(this.theme.bg_surface)
                .fontColor(this.theme.text_primary)
                .onClick(() => { this.reset(); })
            }
          }
          .width('100%')
        }
        .width('100%')
        .padding(16)
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
