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
 *
 * examples/desktop/src/main/models.ts is a port of this file and is meant to
 * stay one: where one gains a rule the other should.
 */
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import hilog from '@ohos.hilog';
import yoai from 'libyoai.so';
import { ModelConfig, isLocal, modelId } from '../model/Provider';
import { ChatSettings } from './ChatSettings';
import { LocalModelStore } from './LocalModelStore';
import { Reader } from './Reader';
import { SecureKeyStore } from './SecureKeyStore';

const STORE_NAME: string = 'models';
const KEY_LIST: string = 'configured';
const KEY_ACTIVE: string = 'active';

/** Bumped whenever a preload changes state, so a screen showing it re-reads. */
export const LOAD_REV: string = 'modelLoadRev';

/** An on-device model's weights: being read, resident, or refused with a reason. */
export interface ModelLoadState {
  state: string;     // 'loading' | 'ready' | 'failed'
  reason: string;    // why it failed; '' otherwise
}

function log(msg: string): void {
  hilog.info(0x0000, 'yoai', '%{public}s', msg);
}

export class ModelStore {
  private static store: preferences.Preferences | null = null;

  /**
   * How each automatic preload went, for the models page to show beside the
   * model. Only models a preload was started for appear: the active one, at
   * startup and when it is switched to.
   */
  private static loadState: Map<string, ModelLoadState> = new Map<string, ModelLoadState>();

  /** Loads in flight, by id, so a second ask waits on the first instead of loading twice. */
  private static inflight: Map<string, Promise<boolean>> = new Map<string, Promise<boolean>>();

  private static async prefs(ctx: common.Context): Promise<preferences.Preferences> {
    if (ModelStore.store === null) {
      ModelStore.store = await preferences.getPreferences(ctx, STORE_NAME);
    }
    return ModelStore.store;
  }

  /** Every model the user has configured, in the order they added them. */
  static async list(ctx: common.Context): Promise<ModelConfig[]> {
    const p: preferences.Preferences = await ModelStore.prefs(ctx);
    const raw: string = await p.get(KEY_LIST, '[]') as string;
    try {
      return JSON.parse(raw) as ModelConfig[];
    } catch (err) {
      // Corrupt JSON is not worth crashing over, but it IS worth saying out
      // loud: silently returning [] would look identical to a fresh install.
      hilog.error(0x0000, 'yoai', 'model list unreadable, starting empty');
      return [];
    }
  }

  private static async write(ctx: common.Context, list: ModelConfig[]): Promise<void> {
    const p: preferences.Preferences = await ModelStore.prefs(ctx);
    await p.put(KEY_LIST, JSON.stringify(list));
    await p.flush();
  }

  /**
   * Add or replace one model, and register it with the core so it is usable
   * immediately -- without this the user would have to restart the app after
   * adding a model, which is the kind of thing nobody reports as a bug because
   * they assume it is how the app works.
   *
   * Selected when nothing is. Adding a second model while one is selected must
   * not silently move the conversation to it, but the first one ever added has
   * to be the selection, or the app reports having no model while the picker
   * shows that model's name.
   */
  static async add(ctx: common.Context, cfg: ModelConfig): Promise<void> {
    const list: ModelConfig[] = await ModelStore.list(ctx);
    const next: ModelConfig[] = [];
    for (const m of list) {
      if (m.id !== cfg.id) {
        next.push(m);
      }
    }
    next.push(cfg);
    await ModelStore.write(ctx, next);
    if ((await ModelStore.activeId(ctx)).length === 0) {
      await ModelStore.setActiveId(ctx, cfg.id);
    }
    await ModelStore.register(cfg);
  }

  /** Forget one model. Its vendor's key is left alone -- other models may share it. */
  static async remove(ctx: common.Context, id: string): Promise<void> {
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
    yoai.nativeProviderRemove(id);
    ModelStore.loadState.delete(id);
    ModelStore.bump();

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
  static async removeVendor(ctx: common.Context, vendorId: string): Promise<void> {
    const list: ModelConfig[] = await ModelStore.list(ctx);
    for (const m of list) {
      if (m.vendorId === vendorId) {
        await ModelStore.remove(ctx, m.id);
      }
    }
    await SecureKeyStore.deleteKey(vendorId);
  }

  /** The model the composer is pointed at, or '' when nothing is configured. */
  static async activeId(ctx: common.Context): Promise<string> {
    const p: preferences.Preferences = await ModelStore.prefs(ctx);
    return await p.get(KEY_ACTIVE, '') as string;
  }

  static async setActiveId(ctx: common.Context, id: string): Promise<void> {
    const p: preferences.Preferences = await ModelStore.prefs(ctx);
    await p.put(KEY_ACTIVE, id);
    await p.flush();
  }

  /**
   * Register one model with the core. For a remote model that means reading its
   * key from the secure store; for an on-device one, handing over the file path.
   *
   * Returns false when the model cannot run -- no key for its vendor, or no path.
   * The core is then left WITHOUT the entry, deliberately: yoai_providers() is
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
      // and the app has no settings screen for them. See src/yoai.h if that
      // changes.
      const rc: number = yoai.nativeLocalSet(cfg.id, path, '');
      if (rc !== 0) {
        hilog.error(0x0000, 'yoai', 'nativeLocalSet(%{public}s) failed rc=%{public}d', cfg.id, rc);
        return false;
      }
      return true;
    }

    const key: string = await SecureKeyStore.getKey(cfg.vendorId);
    if (key.length === 0) {
      return false;
    }
    const rc: number = yoai.nativeProviderSet(cfg.id, cfg.baseUrl, key, cfg.model, '');
    if (rc !== 0) {
      hilog.error(0x0000, 'yoai', 'nativeProviderSet(%{public}s) failed rc=%{public}d', cfg.id, rc);
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
  static async syncToCore(ctx: common.Context): Promise<number> {
    yoai.nativeProviderClear();

    let list: ModelConfig[] = await ModelStore.list(ctx);
    let active: string = await ModelStore.activeId(ctx);

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
        hilog.warn(0x0000, 'yoai', 'dropping %{public}d unreachable on-device model(s)',
                   gone.length);
        await ModelStore.write(ctx, kept);
        list = kept;
        if (gone.includes(active)) {
          active = '';
        }
      }
    }

    // Heal a selection that never got made, or that pointed at a model just
    // dropped. add() selects now, but a store written before it did has models
    // and an empty `active`, and nothing else would ever fill it.
    if (active.length === 0 && list.length > 0) {
      await ModelStore.setActiveId(ctx, list[list.length - 1].id);
    }

    let ready: number = 0;
    for (const m of list) {
      if (await ModelStore.register(m)) {
        ready++;
      }
    }
    hilog.info(0x0000, 'yoai', 'registered %{public}d of %{public}d models', ready, list.length);
    return ready;
  }

  //----------------------------------------------------------------------------
  // On-device engines
  //----------------------------------------------------------------------------

  /** A snapshot of how each preload went, by model id. */
  static loadStates(): Map<string, ModelLoadState> {
    return new Map<string, ModelLoadState>(ModelStore.loadState);
  }

  private static bump(): void {
    const rev: number | undefined = AppStorage.get<number>(LOAD_REV);
    AppStorage.setOrCreate(LOAD_REV, (rev === undefined ? 0 : rev) + 1);
  }

  /**
   * Read the selected on-device model's weights now, without being asked.
   *
   * Only the ACTIVE one. Every configured local model is gigabytes, and loading
   * a model nobody selected would spend the memory of a conversation that is not
   * happening. The first turn pays for the weights otherwise, which is the one
   * moment the user is already waiting.
   *
   * Resolves to whether the active model is resident -- true at once when it
   * already is, false for a remote model or one that would not load -- so a
   * caller that needs the weights can wait for them; the screens do not, and
   * read the outcome from loadStates().
   */
  static async autoPreload(ctx: common.Context): Promise<boolean> {
    const id: string = await ModelStore.activeId(ctx);
    let cfg: ModelConfig | undefined = undefined;
    for (const m of await ModelStore.list(ctx)) {
      if (m.id === id) {
        cfg = m;
      }
    }
    if (cfg === undefined || !isLocal(cfg)) {
      return false;
    }
    const st: ModelLoadState | undefined = ModelStore.loadState.get(id);
    if (st !== undefined && st.state === 'ready') {
      return true;
    }
    const running: Promise<boolean> | undefined = ModelStore.inflight.get(id);
    if (running !== undefined) {
      return running;
    }
    const load: Promise<boolean> = ModelStore.preload(ctx, id, cfg.modelPath ?? '');
    ModelStore.inflight.set(id, load);
    return load;
  }

  /**
   * The file read once first, then the load. Mapped straight from user storage,
   * a 2.74 GB model froze the ArkTS thread for 9.5s and the system killed the
   * app; see nativePrefetch.
   */
  private static async preload(ctx: common.Context, id: string, path: string): Promise<boolean> {
    log('preloading ' + id);
    ModelStore.loadState.set(id, { state: 'loading', reason: '' });
    ModelStore.bump();
    if (path.length > 0) {
      await LocalModelStore.prefetch(path);
    }
    const ok: boolean = await new Promise<boolean>((resolve: (ok: boolean) => void) => {
      yoai.nativeLocalLoad(id, (type: string, content: string) => {
        if (type === 'end') {
          log(id + ' is resident');
          ModelStore.loadState.set(id, { state: 'ready', reason: '' });
        } else {
          hilog.error(0x0000, 'yoai', '%{public}s would not preload: %{public}s', id, content);
          ModelStore.loadState.set(id, {
            state: 'failed',
            reason: content.length > 0 ? content : 'the model would not load'
          });
        }
        ModelStore.bump();
        resolve(type === 'end');
      });
    });
    ModelStore.inflight.delete(id);
    if (ok) {
      ModelStore.warmActive(ctx);
    }
    return ok;
  }

  /**
   * Compute the active on-device model's prompt front now: the system prompt the
   * next turn will be sent with, from ChatSettings.turnPrompt as the turn takes
   * it.
   *
   * The first turn of a conversation otherwise prefills it while someone waits
   * -- measured at 3.5s on the desktop for a 236-token prompt, before the
   * reply's first word. Called whenever that front would otherwise be missing or
   * stale: after the weights load, after a new conversation drops the cache,
   * when the system prompt changes, and when the voice comes up or goes, which
   * adds or removes the spoken-style paragraph.
   *
   * Fire and forget. A turn that arrives meanwhile waits for it in the core and
   * then reuses it; one that finds a different prompt reuses what they share.
   */
  static async warmActive(ctx: common.Context): Promise<void> {
    const id: string = await ModelStore.activeId(ctx);
    const st: ModelLoadState | undefined = ModelStore.loadState.get(id);
    if (st === undefined || st.state !== 'ready') {
      return;
    }
    const prompt: string = await ChatSettings.turnPrompt(ctx, Reader.ready());
    yoai.nativeLocalWarm(id, prompt, (type: string, content: string) => {
      if (type !== 'end') {
        hilog.warn(0x0000, 'yoai', '%{public}s: the prompt was not warmed: %{public}s', id, content);
      }
    });
  }

  /**
   * Forget every on-device model's conversation. The weights stay resident.
   *
   * Called when a new conversation starts, and not once per turn: a turn already
   * carries its whole transcript, so the only moment the cache holds something
   * the next prompt has no business seeing is this one.
   *
   * All of them rather than the active one, because switching models is a way
   * to leave a conversation behind too, and the one being left is the one that
   * still has it.
   */
  static async forgetAll(ctx: common.Context): Promise<number> {
    let cleared: number = 0;
    for (const cfg of await ModelStore.list(ctx)) {
      if (isLocal(cfg) && yoai.nativeLocalForget(cfg.id) === 0) {
        cleared++;
      }
    }
    // The system prompt went with the conversation; put it back before the new
    // conversation's first turn needs it.
    ModelStore.warmActive(ctx);
    return cleared;
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
