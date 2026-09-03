/**
 * Interface preferences: light/dark, theme color, text size, language.
 *
 * Two of these look like one control and are not. Light/dark/system is a MODE;
 * theme color is a FAMILY of two palettes, one light and one dark. Together
 * they name one of the six palettes in [Palettes] -- which is why the color
 * swatches on each family row redraw when the mode row changes, and why
 * neither control alone tells you what you will get.
 *
 * Text size rides the `fontScale` prop that Index applies to conversation
 * content and nothing else; see the note in [Interface].
 */
import { common } from '@kit.AbilityKit';
import { promptAction, router } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { i18n, intl } from '@kit.LocalizationKit';
import ConfigurationConstant from '@ohos.app.ability.ConfigurationConstant';
import { Language, LANGUAGES, TEXT_SIZES, TextSizeStep, languageIndexFor, stepIndexFor } from '../core/Interface';
import { DEFAULT_FAMILY, FAMILIES, Family, Palette, resolvePalette } from '../core/Palettes';

/** One row of the light/dark control. */
interface ThemeMode {
  mode: number;
  label: Resource;
}

/** "Follow system" first: it is the default, and the other two are overrides. */
const MODES: ThemeMode[] = [
  {
    mode: ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET,
    label: $r('app.string.theme_system')
  },
  {
    mode: ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT,
    label: $r('app.string.theme_light')
  },
  {
    mode: ConfigurationConstant.ColorMode.COLOR_MODE_DARK,
    label: $r('app.string.theme_dark')
  }
];

@Entry
@Component
struct SettingsInterface {
  @StorageLink('colorModePref') colorModePref: number =
    ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET;

  /** Scale for conversation text. Index applies it; see [Interface]. */
  @StorageLink('fontScale') fontScale: number = 1;

  /** Row in LANGUAGES matching the app's preferred language, or -1 for none. */
  @State private langIndex: number = -1;

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

  private applyColorMode(mode: number): void {
    this.colorModePref = mode;
    this.ctx().getApplicationContext().setColorMode(mode as ConfigurationConstant.ColorMode);

    // Resolve the palette here as well. setColorMode does not take effect
    // synchronously, so waiting for onConfigurationUpdate leaves the palette a
    // beat behind the row the user just tapped.
    //
    // Nothing to do for NOT_SET: the app has just handed the decision back to
    // the system, and onConfigurationUpdate is the only thing that knows what
    // the system currently says.
    if (mode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK) {
      AppStorage.setOrCreate('isDark', true);
    } else if (mode === ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT) {
      AppStorage.setOrCreate('isDark', false);
    }
  }

  /**
   * Writing the @StorageLink is the whole of it: the prop is persisted, and
   * Index reads the same one, so the conversation resizes without this page
   * telling it anything.
   */
  private applyTextScale(scale: number): void {
    this.fontScale = scale;
  }

  /**
   * setAppPreferredLanguage persists on its own and re-resolves resources, so
   * there is nothing to store here and nothing to re-apply at launch.
   *
   * Layout is a separate matter: for the two RTL languages the strings flip but
   * the layout does not, because this app still positions with left/right
   * padding rather than start/end. Choosing Arabic or Hebrew today gets you
   * mirrored text in an unmirrored frame.
   */
  private applyLanguage(index: number): void {
    const lang: Language = LANGUAGES[index];
    try {
      i18n.System.setAppPreferredLanguage(lang.tag);
      this.langIndex = index;
    } catch (err) {
      // Nothing actionable for the user -- the picker simply does not move.
      const e: BusinessError = err as BusinessError;
      promptAction.showToast({ message: `${lang.label}: ${e.message}` });
    }
  }

  //----------------------------------------------------------------------------
  // Views
  //----------------------------------------------------------------------------

  @Builder
  sectionTitle(text: ResourceStr) {
    Text(text)
      .fontSize(13)
      .fontColor(this.theme.text_tertiary)
      .width('100%')
      .padding({ left: 16, right: 16, top: 18, bottom: 6 })
  }

  /**
   * Light / dark / follow system, as a list of rows like every other choice on
   * this page.
   *
   * It used to be a row with a dropdown, which made it look like a different
   * kind of setting than the three below it when it is the same kind: pick one
   * of a few. It is also the control the theme-color rows depend on, so hiding
   * its options behind a tap was the wrong one to hide.
   *
   * MODES is built here rather than in [Interface] because the values are
   * ConfigurationConstant members -- putting them in a shared module would
   * make it import an ability API for three constants.
   */
  @Builder
  themeModeSection() {
    Column({ space: 8 }) {
      ForEach(MODES, (m: ThemeMode) => {
        Row({ space: 12 }) {
          Text(m.label)
            .fontSize(15)
            .fontWeight(FontWeight.Medium)
            .fontColor(this.theme.text_primary)
            .layoutWeight(1)
          if (this.colorModePref === m.mode) {
            this.tick()
          }
        }
        .width('100%')
        .height(48)
        .padding({ left: 13, right: 13 })
        .borderRadius(12)
        .backgroundColor(this.colorModePref === m.mode
          ? this.theme.bg_bubble
          : this.theme.bg_page)
        .border({
          width: 1,
          color: this.colorModePref === m.mode ? this.theme.brand : this.theme.border
        })
        .onClick(() => this.applyColorMode(m.mode))
      }, (m: ThemeMode) => m.mode.toString())
    }
    .width('100%')
    .padding(16)
  }

  /**
   * The timestamp in the text-size preview.
   *
   * Formatted rather than written out. "12:40" is not a time in every language:
   * hour-minute order, 12- against 24-hour, the separator and the digit shapes
   * all move with the locale, and an Arabic interface wants Arabic-Indic
   * digits. Formatted against the APP's language, not the device's, because
   * that is what the picker below this changes.
   *
   * The current time rather than a sample one, so the preview also shows the
   * real thing a conversation row would carry.
   */
  private sampleTime(): string {
    const pref: string = i18n.System.getAppPreferredLanguage();
    const locale: string = (pref.length === 0 || pref === 'default')
      ? i18n.System.getSystemLocale()
      : pref;
    const fmt: intl.DateTimeFormat =
      new intl.DateTimeFormat(locale, { hour: '2-digit', minute: '2-digit' });
    return fmt.format(new Date());
  }

  /**
   * The selected-row tick.
   *
   * Drawn rather than loaded from a media resource because its color is the
   * palette's accent, which now varies with the user's choice -- Amber's is
   * #E2701E, Arctic's #0072E8. A base/dark SVG pair can follow light and dark
   * but not that, so it would be wrong in four of the six palettes. Shape's
   * viewPort does the job an SVG viewBox would; the commands are Lucide's
   * `check` unchanged, per design/icons.md.
   */
  @Builder
  tick() {
    Shape() {
      // 2, not the 1.5 in design/icons.md: Shape scales the stroke with the
      // geometry, and this 24 viewPort is drawn at 18, so 2 x 18/24 renders as
      // exactly the 1.5 the spec asks for. Writing 1.5 here would draw 1.125.
      Path({ commands: 'M20 6 9 17l-5-5' })
        .stroke(this.theme.brand)
        .strokeWidth(2)
        .strokeLineCap(LineCapStyle.Round)
        .strokeLineJoin(LineJoinStyle.Round)
        .fill(Color.Transparent)
    }
    .viewPort({ x: 0, y: 0, width: 24, height: 24 })
    .width(18)
    .height(18)
  }

  /**
   * The color family. Three rows, not six palettes: mode is the row above, and
   * the two together name one of the six.
   *
   * Each chip previews the member that is actually in force, so switching the
   * mode row restyles all three chips -- which is the honest thing to show,
   * since that is what picking the family would get you right now.
   */
  @Builder
  paletteSection() {
    Column({ space: 8 }) {
      ForEach(FAMILIES, (f: Family) => {
        Row({ space: 12 }) {
          Row() {
            ForEach(resolvePalette(f.key, this.isDark).swatches, (c: string, i: number) => {
              // All four corners always given a number: a ternary that returns
              // an object in one branch and a number in the other is a union
              // ArkTS will not accept here.
              Column()
                .width(12)
                .height(24)
                .backgroundColor(c)
                .borderRadius({
                  topLeft: i === 0 ? 5 : 0,
                  bottomLeft: i === 0 ? 5 : 0,
                  topRight: i === 2 ? 5 : 0,
                  bottomRight: i === 2 ? 5 : 0
                })
            }, (c: string, i: number) => `${f.key}-${i}`)
          }
          .flexShrink(0)

          Text(f.label)
            .fontSize(15)
            .fontWeight(FontWeight.Medium)
            .fontColor(this.theme.text_primary)
            .layoutWeight(1)

          if (this.paletteFamily === f.key) {
            this.tick()
          }
        }
        .width('100%')
        .height(48)
        .padding({ left: 13, right: 13 })
        .borderRadius(12)
        .backgroundColor(this.paletteFamily === f.key
          ? this.theme.bg_bubble
          : this.theme.bg_page)
        .border({
          width: 1,
          color: this.paletteFamily === f.key ? this.theme.brand : this.theme.border
        })
        // Writing the @StorageLink is all of it: Index links the same prop.
        .onClick(() => { this.paletteFamily = f.key; })
      }, (f: Family) => f.key)
    }
    .width('100%')
    .padding(16)
  }

  /**
   * Five named steps, and a preview.
   *
   * A column of named rows rather than the design's row of buttons showing a
   * letter A. Two reasons: a bare A does not say what it does, and five buttons
   * across one card leaves about 60vp each -- enough for two Chinese
   * characters, nowhere near enough for "Smallest" in one of the other ten
   * languages.
   *
   * Each row draws its own label at its own step's size, so the control
   * demonstrates itself: the row that says "larger" is larger. That is also
   * why these rows use a minimum height instead of a fixed one -- the tallest
   * step needs more than 48, and a long label in some language may wrap.
   *
   * The preview below puts a fixed-size timestamp next to a scaled sample,
   * which is the split the setting actually makes in the conversation: message
   * text grows, the metadata around it does not.
   */
  @Builder
  textSizeSection() {
    Column({ space: 12 }) {
      Column({ space: 8 }) {
        ForEach(TEXT_SIZES, (s: TextSizeStep, i: number) => {
          Row({ space: 12 }) {
            Text(s.label)
              .fontSize(s.sample)
              .fontWeight(FontWeight.Medium)
              .fontColor(this.theme.text_primary)
              .layoutWeight(1)
            if (stepIndexFor(this.fontScale) === i) {
              this.tick()
            }
          }
          .width('100%')
          .constraintSize({ minHeight: 48 })
          .padding({ left: 13, right: 13, top: 8, bottom: 8 })
          .borderRadius(12)
          .backgroundColor(stepIndexFor(this.fontScale) === i
            ? this.theme.bg_bubble
            : this.theme.bg_page)
          .border({
            width: 1,
            color: stepIndexFor(this.fontScale) === i
              ? this.theme.brand
              : this.theme.border
          })
          .onClick(() => this.applyTextScale(s.scale))
        }, (s: TextSizeStep) => s.scale.toString())
      }
      .width('100%')

      Column({ space: 6 }) {
        Text($r('app.string.settings_text_preview'))
          .fontSize(11)
          .fontColor(this.theme.text_tertiary)
          .width('100%')
        Row({ space: 12 }) {
          Text(this.sampleTime())
            .fontSize(11)
            .fontColor(this.theme.text_tertiary)
            .flexShrink(0)
          Text($r('app.string.settings_text_sample'))
            .fontSize(16 * this.fontScale)
            .fontColor(this.theme.text_primary)
            .layoutWeight(1)
        }
        .width('100%')
        .alignItems(VerticalAlign.Top)
      }
      .width('100%')
      .padding(14)
      .borderRadius(12)
      .border({ width: 1, color: this.theme.border })
      .backgroundColor(this.theme.bg_page)

      Text($r('app.string.settings_text_size_note'))
        .fontSize(12)
        .fontColor(this.theme.text_tertiary)
        .width('100%')
    }
    .width('100%')
    .padding(16)
  }

  /**
   * Every row is labelled in its own language, never a translation -- see the
   * note on [Language]. That is also why the labels are plain strings and not
   * string resources: they must stay readable no matter which language the rest
   * of the interface is currently in.
   */
  @Builder
  languageSection() {
    Column({ space: 8 }) {
      ForEach(LANGUAGES, (l: Language, i: number) => {
        Row({ space: 12 }) {
          Text(l.label)
            .fontSize(15)
            .fontWeight(FontWeight.Medium)
            .fontColor(this.theme.text_primary)
            .layoutWeight(1)
          if (this.langIndex === i) {
            this.tick()
          }
        }
        .width('100%')
        .height(48)
        .padding({ left: 13, right: 13 })
        .borderRadius(12)
        .backgroundColor(this.langIndex === i
          ? this.theme.bg_bubble
          : this.theme.bg_page)
        .border({
          width: 1,
          color: this.langIndex === i ? this.theme.brand : this.theme.border
        })
        .onClick(() => this.applyLanguage(i))
      }, (l: Language) => l.tag)

      Text($r('app.string.settings_language_note'))
        .fontSize(12)
        .fontColor(this.theme.text_tertiary)
        .width('100%')
        .margin({ top: 4 })
      Text($r('app.string.settings_language_partial'))
        .fontSize(12)
        .fontColor(this.theme.text_tertiary)
        .width('100%')
    }
    .width('100%')
    .padding(16)
  }

  async aboutToAppear(): Promise<void> {
    this.resolveTheme();   // @Watch does not fire on construction; see Index
    this.langIndex = languageIndexFor(i18n.System.getAppPreferredLanguage());
  }

  build() {
    Column() {
      Row({ space: 8 }) {
        Button() {
          Text('\u2039').fontSize(24).fontColor(this.theme.text_secondary)
        }
        .type(ButtonType.Circle)
        .backgroundColor(Color.Transparent)
        .width(40)
        .height(40)
        .onClick(() => router.back())

        Text($r('app.string.settings_interface'))
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
          Text($r('app.string.settings_interface_intro'))
            .fontSize(12)
            .fontColor(this.theme.text_tertiary)
            .width('100%')
            .padding({ left: 16, right: 16, top: 4, bottom: 8 })

          this.sectionTitle($r('app.string.settings_theme'))
          Column() {
            this.themeModeSection()
          }
          .width('100%')
          .margin({ left: 12, right: 12 })
          .borderRadius(12)
          .backgroundColor(this.theme.bg_surface)

          this.sectionTitle($r('app.string.settings_palette'))
          Column() {
            this.paletteSection()
            Text($r('app.string.settings_palette_note'))
              .fontSize(12)
              .fontColor(this.theme.text_tertiary)
              .width('100%')
              .padding({ left: 16, right: 16, bottom: 16 })
          }
          .width('100%')
          .margin({ left: 12, right: 12 })
          .borderRadius(12)
          .backgroundColor(this.theme.bg_surface)

          this.sectionTitle($r('app.string.settings_text_size'))
          Column() {
            this.textSizeSection()
          }
          .width('100%')
          .margin({ left: 12, right: 12 })
          .borderRadius(12)
          .backgroundColor(this.theme.bg_surface)

          this.sectionTitle($r('app.string.settings_language'))
          Column() {
            this.languageSection()
          }
          .width('100%')
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
