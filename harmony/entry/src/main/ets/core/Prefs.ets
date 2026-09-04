/**
 * The three interface preferences, on disk.
 *
 * Written with `preferences`, not `PersistentStorage`, after the latter was
 * measured on device and does not read back what it writes. Its file appeared
 * at files/persistent_storage with the right contents after a change, and on
 * the next cold start persistProp handed out the DEFAULTS instead -- for all
 * three props, including the plain string one, so it was not a type mismatch.
 * Registering in onWindowStageCreate instead of onCreate made no difference,
 * and neither did reading two seconds after the first page had loaded, so it
 * was not a timing or async-load race either.
 *
 * Not a mistake local to this app: another HarmonyOS app on the same phone
 * persists the same two props the same way and also comes up light with
 * colorModePref=0 on disk. So the remedy below is settled and the cause is not.
 *
 * `preferences` is also what ModelStore already uses in this app, and it
 * survives a restart.
 *
 * AppStorage is still the channel to the pages: [load] fills it before the
 * first page is built, and the pages keep their @StorageLink. What changed is
 * only where the values come from and who writes them back -- each setter
 * calls the matching save here, because AppStorage has no change hook outside
 * a component.
 */
import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import ConfigurationConstant from '@ohos.app.ability.ConfigurationConstant';
import { DEFAULT_FAMILY } from './Palettes';

const STORE_NAME: string = 'interface';

export const KEY_COLOR_MODE: string = 'colorModePref';
export const KEY_FONT_SCALE: string = 'fontScale';
export const KEY_PALETTE_FAMILY: string = 'paletteFamily';

export class Prefs {
  private static store: preferences.Preferences | null = null;

  private static async prefs(ctx: common.UIAbilityContext): Promise<preferences.Preferences> {
    if (Prefs.store === null) {
      Prefs.store = await preferences.getPreferences(ctx, STORE_NAME);
    }
    return Prefs.store;
  }

  /**
   * Read the three into AppStorage.
   *
   * Await this before loading the first page: a page built while these are
   * still absent binds its @StorageLink to the declared default, which is a
   * visible flash of the light theme for someone who chose dark.
   */
  static async load(ctx: common.UIAbilityContext): Promise<void> {
    const p: preferences.Preferences = await Prefs.prefs(ctx);
    const mode: number =
      await p.get(KEY_COLOR_MODE, ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET) as number;
    const scale: number = await p.get(KEY_FONT_SCALE, 1) as number;
    const family: string = await p.get(KEY_PALETTE_FAMILY, DEFAULT_FAMILY) as string;

    AppStorage.setOrCreate(KEY_COLOR_MODE, mode);
    AppStorage.setOrCreate(KEY_FONT_SCALE, scale);
    AppStorage.setOrCreate(KEY_PALETTE_FAMILY, family);
  }

  /** Persist one value. The caller has already written it to AppStorage. */
  static async save(ctx: common.UIAbilityContext, key: string, value: number | string): Promise<void> {
    const p: preferences.Preferences = await Prefs.prefs(ctx);
    await p.put(key, value);
    await p.flush();
  }
}
