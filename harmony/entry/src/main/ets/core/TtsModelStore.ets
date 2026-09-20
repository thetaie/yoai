/**
 * Remembering which two .gguf files are the speech model.
 *
 * TWO references, and that is the only real difference from VoiceModelStore: a
 * Qwen3-TTS model is a talker plus a 12 Hz audio tokenizer, and neither is
 * derivable from the other — different repositories, different sizes, different
 * names. Picking them is two trips through the picker, and re-picking both on
 * every launch would make the lane unusable, so both are stored.
 *
 * What is stored is a REFERENCE, never a copy — the same rule as
 * LocalModelStore and for the same reason. The uri is what the grant is keyed
 * on and the path is what native opens; both are needed and neither is
 * derivable from the other at load time.
 *
 * It shares the `models` preferences store with the other two lanes. A third
 * store file for two keys would be ceremony.
 */
import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import hilog from '@ohos.hilog';

const STORE_NAME: string = 'models';
const KEY_TALKER: string = 'ttsTalker';
const KEY_CODEC: string = 'ttsCodec';

/** One stored reference. Same three fields PickedFile carries that outlive it. */
export interface TtsModelRef {
  uri: string;
  path: string;
  name: string;
}

/** Both halves, either of which may be missing until it has been picked. */
export interface TtsModelPair {
  talker?: TtsModelRef;
  codec?: TtsModelRef;
}

function log(msg: string): void {
  hilog.info(0x0000, 'bodyclock', '%{public}s', msg);
}

export class TtsModelStore {
  private static store: preferences.Preferences | null = null;

  private static async prefs(ctx: common.UIAbilityContext): Promise<preferences.Preferences> {
    if (TtsModelStore.store === null) {
      TtsModelStore.store = await preferences.getPreferences(ctx, STORE_NAME);
    }
    return TtsModelStore.store;
  }

  private static async read(ctx: common.UIAbilityContext,
                            key: string): Promise<TtsModelRef | undefined> {
    const p: preferences.Preferences = await TtsModelStore.prefs(ctx);
    const raw: string = await p.get(key, '') as string;
    if (raw.length === 0) {
      return undefined;
    }
    try {
      const ref: TtsModelRef = JSON.parse(raw) as TtsModelRef;
      if (ref.uri === undefined || ref.path === undefined) {
        return undefined;
      }
      return ref;
    } catch (err) {
      // A malformed entry is not worth recovering; treat it as absent so the
      // next pick overwrites it.
      log('stored tts model ref unreadable: ' + key);
      return undefined;
    }
  }

  /** Both remembered halves. Either may be undefined. */
  static async get(ctx: common.UIAbilityContext): Promise<TtsModelPair> {
    const talker: TtsModelRef | undefined = await TtsModelStore.read(ctx, KEY_TALKER);
    const codec: TtsModelRef | undefined = await TtsModelStore.read(ctx, KEY_CODEC);
    return { talker: talker, codec: codec };
  }

  static async setTalker(ctx: common.UIAbilityContext, ref: TtsModelRef): Promise<void> {
    const p: preferences.Preferences = await TtsModelStore.prefs(ctx);
    await p.put(KEY_TALKER, JSON.stringify(ref));
    await p.flush();
  }

  static async setCodec(ctx: common.UIAbilityContext, ref: TtsModelRef): Promise<void> {
    const p: preferences.Preferences = await TtsModelStore.prefs(ctx);
    await p.put(KEY_CODEC, JSON.stringify(ref));
    await p.flush();
  }

  /**
   * Forget both. Called when a file has moved or been deleted — keeping a
   * reference that fails on every launch turns one lost file into a permanent
   * error message.
   *
   * The grants are not revoked here. They cost nothing to leave, and revoking a
   * path that already failed to resolve is an error handling an error.
   */
  static async clear(ctx: common.UIAbilityContext): Promise<void> {
    const p: preferences.Preferences = await TtsModelStore.prefs(ctx);
    await p.delete(KEY_TALKER);
    await p.delete(KEY_CODEC);
    await p.flush();
  }
}
