/**
 * The user's configured models, and the one they are chatting with.
 *
 * Two stores behind one door, because a key and a model name deserve different
 * treatment: the non-secret part (vendor, base URL, model name, which one is
 * selected) lives in preferences, and the key lives in [SecureKeyStore]. Nothing
 * here ever holds a key in a field -- it is read from the secure store at the
 * moment it is handed to the core, and not kept.
 *
 * The core's registry is process memory, so it is empty after every cold start.
 * syncToCore() is what refills it, and is the reason this class knows about the
 * native module at all.
 */
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import hilog from '@ohos.hilog';
import bodyclock from 'libbodyclock.so';
import { ModelConfig, isLocal, modelId } from '../model/Provider';
import { LocalModelStore } from './LocalModelStore';
import { SecureKeyStore } from './SecureKeyStore';

const STORE_NAME: string = 'models';
const KEY_LIST: string = 'configured';
const KEY_ACTIVE: string = 'active';

export class ModelStore {
  private static store: preferences.Preferences | null = null;

  private static async prefs(ctx: common.UIAbilityContext): Promise<preferences.Preferences> {
    if (ModelStore.store === null) {
      ModelStore.store = await preferences.getPreferences(ctx, STORE_NAME);
    }
    return ModelStore.store;
  }

  /** Every model the user has configured, in the order they added them. */
  static async list(ctx: common.UIAbilityContext): Promise<ModelConfig[]> {
    const p: preferences.Preferences = await ModelStore.prefs(ctx);
    const raw: string = await p.get(KEY_LIST, '[]') as string;
    try {
      return JSON.parse(raw) as ModelConfig[];
    } catch (err) {
      // Corrupt JSON is not worth crashing over, but it IS worth saying out
      // loud: silently returning [] would look identical to a fresh install.
      hilog.error(0x0000, 'bodyclock', 'model list unreadable, starting empty');
      return [];
    }
  }

  private static async write(ctx: common.UIAbilityContext, list: ModelConfig[]): Promise<void> {
    const p: preferences.Preferences = await ModelStore.prefs(ctx);
    await p.put(KEY_LIST, JSON.stringify(list));
    await p.flush();
  }

  /**
   * Add or replace one model, and register it with the core so it is usable
   * immediately -- without this the user would have to restart the app after
   * adding a model, which is the kind of thing nobody reports as a bug because
   * they assume it is how the app works.
   */
  static async add(ctx: common.UIAbilityContext, cfg: ModelConfig): Promise<void> {
    const list: ModelConfig[] = await ModelStore.list(ctx);
    const next: ModelConfig[] = [];
    for (const m of list) {
      if (m.id !== cfg.id) {
        next.push(m);
      }
    }
    next.push(cfg);
    await ModelStore.write(ctx, next);
    await ModelStore.register(cfg);
  }

  /** Forget one model. Its vendor's key is left alone -- other models may share it. */
  static async remove(ctx: common.UIAbilityContext, id: string): Promise<void> {
    const list: ModelConfig[] = await ModelStore.list(ctx);
    const next: ModelConfig[] = [];
    for (const m of list) {
      if (m.id !== id) {
        next.push(m);
      } else if (isLocal(m)) {
        // Hand back the file grant we are no longer using. The .gguf itself is
        // the user's and stays exactly where it is.
        await LocalModelStore.release(m);
      }
    }
    await ModelStore.write(ctx, next);
    bodyclock.nativeProviderRemove(id);

    // The selection may have just pointed at it.
    const active: string = await ModelStore.activeId(ctx);
    if (active === id) {
      await ModelStore.setActiveId(ctx, next.length > 0 ? next[next.length - 1].id : '');
    }
  }

  /**
   * Drop every model belonging to a vendor, and that vendor's key. What the
   * settings screen calls "remove this service": leaving models behind whose
   * key is gone would list entries that can only fail.
   */
  static async removeVendor(ctx: common.UIAbilityContext, vendorId: string): Promise<void> {
    const list: ModelConfig[] = await ModelStore.list(ctx);
    for (const m of list) {
      if (m.vendorId === vendorId) {
        await ModelStore.remove(ctx, m.id);
      }
    }
    await SecureKeyStore.deleteKey(vendorId);
  }

  /** The model the composer is pointed at, or '' when nothing is configured. */
  static async activeId(ctx: common.UIAbilityContext): Promise<string> {
    const p: preferences.Preferences = await ModelStore.prefs(ctx);
    return await p.get(KEY_ACTIVE, '') as string;
  }

  static async setActiveId(ctx: common.UIAbilityContext, id: string): Promise<void> {
    const p: preferences.Preferences = await ModelStore.prefs(ctx);
    await p.put(KEY_ACTIVE, id);
    await p.flush();
  }

  /**
   * Register one model with the core. For a remote model that means reading its
   * key from the secure store; for an on-device one, handing over the file path.
   *
   * Returns false when the model cannot run -- no key for its vendor, or no path.
   * The core is then left WITHOUT the entry, deliberately: bodyclock_providers() is
   * the picker's source of truth, so an entry that cannot run must not appear
   * in it.
   */
  static async register(cfg: ModelConfig): Promise<boolean> {
    if (isLocal(cfg)) {
      const path: string | undefined = cfg.modelPath;
      if (path === undefined || path.length === 0) {
        return false;
      }
      // '' for the options: the core's defaults are already tuned for a phone,
      // and the app has no settings screen for them. See src/bodyclock.h if that
      // changes.
      const rc: number = bodyclock.nativeLocalSet(cfg.id, path, '');
      if (rc !== 0) {
        hilog.error(0x0000, 'bodyclock', 'nativeLocalSet(%{public}s) failed rc=%{public}d', cfg.id, rc);
        return false;
      }
      return true;
    }

    const key: string = await SecureKeyStore.getKey(cfg.vendorId);
    if (key.length === 0) {
      return false;
    }
    const rc: number = bodyclock.nativeProviderSet(cfg.id, cfg.baseUrl, key, cfg.model, '');
    if (rc !== 0) {
      hilog.error(0x0000, 'bodyclock', 'nativeProviderSet(%{public}s) failed rc=%{public}d', cfg.id, rc);
      return false;
    }
    return true;
  }

  /**
   * Refill the core's registry from what is stored. Call once at startup, before
   * the first turn. Returns how many models are actually usable.
   *
   * On-device models are re-armed FIRST: their file grants are dormant after a
   * cold start, and registering a path we can no longer open would put an entry
   * in the picker that fails only when someone tries to use it. Ones whose file
   * has moved or gone are dropped from the stored list here -- the single place
   * that owns that list, so an entry cannot vanish from two directions at once.
   */
  static async syncToCore(ctx: common.UIAbilityContext): Promise<number> {
    bodyclock.nativeProviderClear();

    let list: ModelConfig[] = await ModelStore.list(ctx);

    const locals: ModelConfig[] = [];
    for (const m of list) {
      if (isLocal(m)) {
        locals.push(m);
      }
    }
    if (locals.length > 0) {
      const gone: string[] = await LocalModelStore.activate(locals);
      if (gone.length > 0) {
        const kept: ModelConfig[] = [];
        for (const m of list) {
          if (!gone.includes(m.id)) {
            kept.push(m);
          }
        }
        hilog.warn(0x0000, 'bodyclock', 'dropping %{public}d unreachable on-device model(s)',
                   gone.length);
        await ModelStore.write(ctx, kept);
        list = kept;
      }
    }

    let ready: number = 0;
    for (const m of list) {
      if (await ModelStore.register(m)) {
        ready++;
      }
    }
    hilog.info(0x0000, 'bodyclock', 'registered %{public}d of %{public}d models', ready, list.length);
    return ready;
  }

  /** Which vendors have a key stored. Drives the settings screen's state. */
  static async vendorsWithKeys(vendorIds: string[]): Promise<Set<string>> {
    const out: Set<string> = new Set<string>();
    for (const id of vendorIds) {
      if (await SecureKeyStore.hasKey(id)) {
        out.add(id);
      }
    }
    return out;
  }

  /** Convenience for the settings screen: build a config from its parts. */
  static makeConfig(vendorId: string, baseUrl: string, model: string): ModelConfig {
    return {
      id: modelId(vendorId, model),
      vendorId: vendorId,
      baseUrl: baseUrl,
      model: model
    };
  }
}
