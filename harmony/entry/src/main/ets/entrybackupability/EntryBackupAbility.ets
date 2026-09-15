import hilog from '@ohos.hilog';
import BackupExtensionAbility, { BundleVersion } from '@ohos.application.BackupExtensionAbility';

export default class EntryBackupAbility extends BackupExtensionAbility {
  async onBackup() {
    hilog.info(0x0000, 'bodyclock', 'onBackup ok');
  }

  async onRestore(bundleVersion: BundleVersion) {
    hilog.info(0x0000, 'bodyclock', 'onRestore ok %{public}s', JSON.stringify(bundleVersion));
  }
}
