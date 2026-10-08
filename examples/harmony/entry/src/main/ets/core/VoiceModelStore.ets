/**
 * Remembering which .gguf is the recognition model.
 *
 * Deliberately NOT part of the chat model list in ModelStore. A SenseVoice
 * checkpoint is not a chat model: it is picked in a different place, loaded
 * through a different C entry point, and showing it in the model picker
 * alongside things you can talk to would be a lie about what it does. It does
 * share the `models` preferences store, because it is still a model reference
 * and a second store file for one key would be ceremony.
 *
 * What is stored is a REFERENCE, never a copy -- the same rule as
 * LocalModelStore, for the same reason. The uri is what the grant is keyed on
 * and the path is what native opens; both are needed and neither is derivable
 * from the other at load time.
 */
import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import hilog from '@ohos.hilog';

const STORE_NAME: string = 'models';
const KEY_VOICE: string = 'voice';

/** The stored reference. Same three fields PickedFile carries that outlive it. */
export interface VoiceModelRef {
  uri: string;
  path: string;
  name: string;
}

function log(msg: string): void {
  hilog.info(0x0000, 'yoai', '%{public}s', msg);
}

export class VoiceModelStore {
  private static store: preferences.Preferences | null = null;

  private static async prefs(ctx: common.UIAbilityContext): Promise<preferences.Preferences> {
    if (VoiceModelStore.store === null) {
      VoiceModelStore.store = await preferences.getPreferences(ctx, STORE_NAME);
    }
    return VoiceModelStore.store;
  }

  /** The remembered model, or undefined if none was ever picked. */
  static async get(ctx: common.UIAbilityContext): Promise<VoiceModelRef | undefined> {
    const p: preferences.Preferences = await VoiceModelStore.prefs(ctx);
    const raw: string = await p.get(KEY_VOICE, '') as string;
    if (raw.length === 0) {
      return undefined;
    }
    try {
      const ref: VoiceModelRef = JSON.parse(raw) as VoiceModelRef;
      if (ref.uri === undefined || ref.path === undefined) {
        return undefined;
      }
      return ref;
    } catch (err) {
      // A malformed entry is not worth recovering; treat it as absent so the
      // next pick overwrites it.
      log('stored voice model ref unreadable');
      return undefined;
    }
  }

  static async set(ctx: common.UIAbilityContext, ref: VoiceModelRef): Promise<void> {
    const p: preferences.Preferences = await VoiceModelStore.prefs(ctx);
    await p.put(KEY_VOICE, JSON.stringify(ref));
    await p.flush();
  }

  /**
   * Forget it. Called when the file has moved or been deleted -- keeping a
   * reference that fails on every launch turns one lost file into a permanent
   * error message.
   *
   * The grant is not revoked here. It costs nothing to leave, and revoking on a
   * path that already failed to resolve is an error handling an error.
   */
  static async clear(ctx: common.UIAbilityContext): Promise<void> {
    const p: preferences.Preferences = await VoiceModelStore.prefs(ctx);
    await p.delete(KEY_VOICE);
    await p.flush();
  }
}
