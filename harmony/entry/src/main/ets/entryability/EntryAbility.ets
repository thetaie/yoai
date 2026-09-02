import AbilityConstant from '@ohos.app.ability.AbilityConstant';
import hilog from '@ohos.hilog';
import UIAbility from '@ohos.app.ability.UIAbility';
import Want from '@ohos.app.ability.Want';
import window from '@ohos.window';
import ConfigurationConstant from '@ohos.app.ability.ConfigurationConstant';

const TAG: string = 'yoai';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, TAG, '%{public}s', 'Ability onCreate');

    // App-wide font scale, persisted across launches.
    PersistentStorage.persistProp('fontScale', 1);
    // Theme override. Stored as the raw ColorMode value so there is no mapping
    // table: NOT_SET(-1) = follow the system, which is also the first-run default.
    PersistentStorage.persistProp('colorModePref', ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
    this.applyStoredColorMode();
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
