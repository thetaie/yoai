/**
 * Interface preferences: text size, and the interface language.
 *
 * Text size rides the `fontScale` prop that EntryAbility already persists. Its
 * scope is deliberately narrow -- Index applies it to conversation content
 * (both bubbles, the thinking block, the composer) and to nothing else. Chrome
 * keeps fixed sizes, because a header that grows with the user's reading size
 * pushes the conversation off the screen, which is the opposite of what was
 * asked for. Anything added here should follow that split.
 *
 * Language does NOT get a prop of its own: `i18n.System.setAppPreferredLanguage`
 * is itself persistent and survives cold start, so a second copy in
 * PersistentStorage could only ever disagree with it.
 */

/** One step on the text-size control. `scale` multiplies the base font size. */
export interface TextSizeStep {
  scale: number;
  /**
   * Size this step's own row is drawn at in the picker, which is the size
   * conversation text ends up at -- so the row is a sample of its own effect,
   * not an illustration of it.
   */
  sample: number;
  label: Resource;
}

/**
 * Five steps around an unscaled middle.
 *
 * The scales are written as fractions of 16 because 16 is the base size of
 * conversation text -- the only thing this setting touches -- so every step
 * lands on a whole pixel there. Picking round scales instead (0.85, 1.15)
 * would put body text on fractional sizes at every step but the middle.
 */
export const TEXT_SIZES: TextSizeStep[] = [
  { scale: 13 / 16, sample: 13, label: $r('app.string.settings_text_smallest') },
  { scale: 14 / 16, sample: 14, label: $r('app.string.settings_text_smaller') },
  { scale: 1.0, sample: 16, label: $r('app.string.settings_text_normal') },
  { scale: 18 / 16, sample: 18, label: $r('app.string.settings_text_larger') },
  { scale: 20 / 16, sample: 20, label: $r('app.string.settings_text_largest') }
];

/** The middle step: unscaled, and what EntryAbility persists on first run. */
export const DEFAULT_TEXT_SCALE: number = 1.0;

/** Nearest step to a stored scale, so an unknown value still highlights something. */
export function stepIndexFor(scale: number): number {
  let best: number = 0;
  let bestGap: number = Number.MAX_VALUE;
  for (let i = 0; i < TEXT_SIZES.length; i++) {
    const gap: number = Math.abs(TEXT_SIZES[i].scale - scale);
    if (gap < bestGap) {
      bestGap = gap;
      best = i;
    }
  }
  return best;
}

/**
 * An interface language.
 *
 * `label` is the language's own name for itself, never a translation of it: a
 * picker is the one screen a user reaches while the interface is in a language
 * they cannot read, so every row has to be legible before anything is applied.
 * That makes these labels literal strings rather than string resources.
 *
 * `tag` is the BCP-47 tag handed to setAppPreferredLanguage. `dir` is the
 * qualifier directory under resources/ that holds this language's strings.
 */
export interface Language {
  tag: string;
  label: string;
  dir: string;
  rtl: boolean;
}

/**
 * TRANSLATION STATUS: only `base` (Chinese) and `en_US` are written. The other
 * nine qualifier directories exist and are complete key-for-key, but every
 * value in them is still the English one, copied as the source a translator
 * works from. Picking one of those nine today gets you English, not a missing
 * string -- which is the reason they were seeded from en_US rather than left
 * absent, since an absent directory falls back to `base` and would show Chinese.
 */
export const LANGUAGES: Language[] = [
  { tag: 'zh-Hans', label: '中文', dir: 'base', rtl: false },
  { tag: 'en-US', label: 'English', dir: 'en_US', rtl: false },
  { tag: 'fr-FR', label: 'Français', dir: 'fr_FR', rtl: false },
  { tag: 'es-ES', label: 'Español', dir: 'es_ES', rtl: false },
  { tag: 'de-DE', label: 'Deutsch', dir: 'de_DE', rtl: false },
  { tag: 'it-IT', label: 'Italiano', dir: 'it_IT', rtl: false },
  { tag: 'ja-JP', label: '日本語', dir: 'ja_JP', rtl: false },
  { tag: 'ko-KR', label: '한국어', dir: 'ko_KR', rtl: false },
  { tag: 'ru-RU', label: 'Русский', dir: 'ru_RU', rtl: false },
  { tag: 'ar', label: 'العربية', dir: 'ar_AE', rtl: true },
  { tag: 'he', label: 'עברית', dir: 'he_IL', rtl: true }
];

/**
 * Match a preferred-language tag to a row.
 *
 * getAppPreferredLanguage returns whatever the system resolved, which is not
 * always one of the tags above -- 'default' before the user has ever chosen,
 * and a fuller tag ('zh-Hans-CN') once it has resolved against the device. So
 * this compares on the language subtag and treats anything unrecognised as
 * no selection.
 */
export function languageIndexFor(tag: string): number {
  if (tag.length === 0 || tag === 'default') {
    return -1;
  }
  const want: string = tag.split('-')[0].toLowerCase();
  for (let i = 0; i < LANGUAGES.length; i++) {
    if (LANGUAGES[i].tag.split('-')[0].toLowerCase() === want) {
      return i;
    }
  }
  return -1;
}
