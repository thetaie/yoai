import AbilityConstant from '@ohos.app.ability.AbilityConstant';
// Named, not default: this module has no default export, and a default import
// silently resolves to ArkUI's own Configuration -- whose colorMode is a
// string, which then fails to compare against ConfigurationConstant.ColorMode.
import { Configuration } from '@ohos.app.ability.Configuration';
import hilog from '@ohos.hilog';
import UIAbility from '@ohos.app.ability.UIAbility';
import Want from '@ohos.app.ability.Want';
import window from '@ohos.window';
import ConfigurationConstant from '@ohos.app.ability.ConfigurationConstant';
import { DEFAULT_FAMILY } from '../core/Palettes';

const TAG: string = 'yoai';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, TAG, '%{public}s', 'Ability onCreate');

    // App-wide font scale, persisted across launches.
    PersistentStorage.persistProp('fontScale', 1);
    // Theme override. Stored as the raw ColorMode value so there is no mapping
    // table: NOT_SET(-1) = follow the system, which is also the first-run default.
    PersistentStorage.persistProp('colorModePref', ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
    // Which color family the palette comes from. Independent of the mode above:
    // the two together name one of the six palettes in [Palettes].
    PersistentStorage.persistProp('paletteFamily', DEFAULT_FAMILY);
    this.applyStoredColorMode();
    this.publishColorMode(this.context.config.colorMode);
  }

  /**
   * Publish the resolved light/dark state that pages pick their palette with.
   * Derived, so deliberately not persisted.
   *
   * The stored PREFERENCE decides this whenever it is light or dark; the
   * configuration is consulted only for "follow the system". Doing it the other
   * way round -- always reading the configuration -- is wrong on cold start,
   * because setColorMode above does not take effect synchronously: a user whose
   * preference is dark on a light phone would get one frame of the light
   * palette over dark-qualified resources.
   */
  private publishColorMode(systemMode: ConfigurationConstant.ColorMode | undefined): void {
    const stored: number | undefined = AppStorage.get<number>('colorModePref');
    const pref: number = stored === undefined
      ? ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET
      : stored;

    let dark: boolean;
    if (pref === ConfigurationConstant.ColorMode.COLOR_MODE_DARK) {
      dark = true;
    } else if (pref === ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT) {
      dark = false;
    } else {
      dark = systemMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
    }
    AppStorage.setOrCreate('isDark', dark);
  }

  /**
   * Fires when the system theme changes, and also when the app's own
   * setColorMode takes effect -- so a user switching to Dark and a user turning
   * their phone dark at dusk both arrive here. The preference still wins; this
   * only supplies the answer for "follow the system".
   */
  onConfigurationUpdate(newConfig: Configuration): void {
    this.publishColorMode(newConfig.colorMode);
  }

  /**
   * Re-apply the user's theme override on launch. setColorMode only affects the
   * running process, so without this the app falls back to the system theme on
   * every cold start.
   */
  private applyStoredColorMode(): void {
    const stored: number | undefined = AppStorage.get<number>('colorModePref');
    const mode: ConfigurationConstant.ColorMode = stored === undefined
      ? ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET
      : stored as ConfigurationConstant.ColorMode;
    if (mode === ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET) {
      return;
    }
    this.context.getApplicationContext().setColorMode(mode);
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(0x0000, TAG, 'Failed to load the content. Cause: %{public}s', JSON.stringify(err) ?? '');
        return;
      }
      hilog.info(0x0000, TAG, '%{public}s', 'Content loaded');
    });
  }

  onDestroy(): void {
    hilog.info(0x0000, TAG, '%{public}s', 'Ability onDestroy');
  }
}
