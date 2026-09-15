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
import { BusinessError } from '@kit.BasicServicesKit';
import { Prefs } from '../core/Prefs';

const TAG: string = 'bodyclock';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, TAG, '%{public}s', 'Ability onCreate');
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

  /**
   * The stored settings are read here, and the first page is not loaded until
   * they are in AppStorage.
   *
   * The await is the point: a page built before the values land binds its
   * @StorageLink to the declared default, so someone who chose dark would see
   * the light theme for a frame and then a jump. One preferences read is
   * cheaper than that.
   */
  onWindowStageCreate(windowStage: window.WindowStage): void {
    Prefs.load(this.context).then(() => {
      this.applyStoredColorMode();
      this.publishColorMode(this.context.config.colorMode);
      this.load(windowStage);
    }).catch((err: BusinessError) => {
      // Show the app rather than nothing: defaults are a worse first run than
      // the user's choice, but they are still a working one.
      hilog.error(0x0000, TAG, 'Failed to read settings: %{public}s', err.message);
      this.load(windowStage);
    });
  }

  private load(windowStage: window.WindowStage): void {
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
