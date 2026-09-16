/**
 * Picking an on-device model, and keeping it openable.
 *
 * HARD CONSTRAINT that shapes all of this: llama.cpp loads a model by PATH and
 * mmaps it (src/llm/local.cpp), so an ArkTS-readable URI or a file descriptor is
 * not enough. And a multi-GB file has no business being copied into app-private
 * storage, where it would be duplicated and then deleted on uninstall.
 *
 * Both are satisfied at once: a file the user grants through DocumentViewPicker
 * IS openable and mmap-able by native code at the path behind its URI -- the
 * granted `file://docs/storage/Users/currentUser/...` resolves to
 * `/storage/Users/currentUser/...`, which native opens successfully.
 *
 * So there is NO COPY STEP anywhere here. A model is a REFERENCE to a file the
 * user owns; removing it from the app never deletes the file.
 *
 * The grant is the fiddly part. It is persisted with @ohos.fileshare (the
 * HarmonyOS analogue of an iOS security-scoped bookmark) and RE-ACTIVATED on
 * launch, which is what makes a picked path still openable after a cold start.
 * That needs ohos.permission.FILE_ACCESS_PERSIST declared in module.json5, or
 * persistPermission answers 201.
 */
import { common } from '@kit.AbilityKit';
import { picker } from '@kit.CoreFileKit';
import { BusinessError } from '@kit.BasicServicesKit';
import fileUri from '@ohos.file.fileuri';
import fileShare from '@ohos.fileshare';
import fs from '@ohos.file.fs';
import hilog from '@ohos.hilog';
import bodyclock from 'libbodyclock.so';
import { ModelConfig, KIND_LOCAL, LOCAL_VENDOR_ID, modelId } from '../model/Provider';

/** What importModel() produced, or nothing when the user cancelled. */
export interface PickedModel {
  config: ModelConfig;
  sizeBytes: number;
}

/**
 * One picked .gguf, before anything decides what it is for.
 *
 * Split out because a chat model and a recognition model are picked the same
 * way and granted the same way, and the grant is the part worth having exactly
 * one copy of.
 */
export interface PickedFile {
  uri: string;
  path: string;
  name: string;
  sizeBytes: number;
}

function log(msg: string): void {
  hilog.info(0x0000, 'bodyclock', '%{public}s', msg);
}

export class LocalModelStore {
  /**
   * Whether the native module has an engine at all. Cached: it answers a
   * compile-time fact, so asking twice cannot give two answers, and the settings
   * screen reads it on every render.
   */
  private static availability: boolean | null = null;

  static available(): boolean {
    if (LocalModelStore.availability === null) {
      LocalModelStore.availability = bodyclock.nativeLocalAvailable();
    }
    // `=== true` rather than returning the field: ArkTS does not narrow a static
    // member from `boolean | null` across the assignment above, so the plain
    // return is a type error even though the value cannot be null by then.
    return LocalModelStore.availability === true;
  }

  //----------------------------------------------------------------------------
  // Importing
  //----------------------------------------------------------------------------

  /**
   * Let the user pick a .gguf. Returns the model config to store, or undefined
   * when they cancelled or the file turned out not to be reachable.
   *
   * The path is verified with statSync before it is handed back: a URI the picker
   * returns is not a promise that the path behind it resolves, and finding that
   * out at load time would surface as a confusing "could not load model" long
   * after the choice was made.
   */
  /**
   * Pick one .gguf, persist the grant, and confirm the path actually resolves.
   *
   * Used for both kinds of model. The statSync is not belt-and-braces: a URI the
   * picker returns is not a promise that the path behind it resolves, and
   * finding that out at load time would surface as a confusing "could not load
   * model" long after the choice was made.
   */
  static async pickGguf(ctx: common.UIAbilityContext): Promise<PickedFile | undefined> {
    const opts: picker.DocumentSelectOptions = new picker.DocumentSelectOptions();
    opts.maxSelectNumber = 1;
    opts.fileSuffixFilters = ['.gguf'];

    let uris: string[] = [];
    try {
      uris = await new picker.DocumentViewPicker(ctx).select(opts);
    } catch (err) {
      const e = err as BusinessError;
      log('gguf picker failed code=' + e.code + ' ' + e.message);
      return undefined;
    }
    if (uris.length === 0) {
      return undefined;   // cancelled
    }

    const uri: string = uris[0];
    await LocalModelStore.persist(uri);

    const parsed: fileUri.FileUri = new fileUri.FileUri(uri);
    const path: string = parsed.path;
    const name: string = parsed.name;

    let size: number = 0;
    try {
      size = fs.statSync(path).size;
    } catch (err) {
      const e = err as BusinessError;
      log('picked file not reachable at ' + path + ' code=' + e.code);
      return undefined;
    }
    return { uri: uri, path: path, name: name, sizeBytes: size };
  }

  static async importModel(ctx: common.UIAbilityContext): Promise<PickedModel | undefined> {
    const picked: PickedFile | undefined = await LocalModelStore.pickGguf(ctx);
    if (picked === undefined) {
      return undefined;
    }
    const uri: string = picked.uri;
    const path: string = picked.path;
    const name: string = picked.name;
    const size: number = picked.sizeBytes;

    const cfg: ModelConfig = {
      id: modelId(LOCAL_VENDOR_ID, name),
      vendorId: LOCAL_VENDOR_ID,
      baseUrl: '',
      model: name,
      kind: KIND_LOCAL,
      modelPath: path,
      modelUri: uri
    };
    log('imported ' + name + ' (' + size + ' bytes) at ' + path);
    return { config: cfg, sizeBytes: size };
  }

  /**
   * Release the grant for a model being removed. The FILE IS NEVER TOUCHED: it
   * belongs to the user, and deleting a multi-GB download because they tapped
   * "remove" in a list would be indefensible.
   */
  static async release(cfg: ModelConfig): Promise<void> {
    const uri: string | undefined = cfg.modelUri;
    if (uri === undefined || uri.length === 0) {
      return;
    }
    try {
      await fileShare.revokePermission([{
        uri: uri, operationMode: fileShare.OperationMode.READ_MODE
      }]);
    } catch (err) {
      // Already gone, or never persisted: there is nothing to release.
      const e = err as BusinessError;
      log('revokePermission code=' + e.code);
    }
  }

  //----------------------------------------------------------------------------
  // Grants
  //----------------------------------------------------------------------------

  private static async persist(uri: string): Promise<void> {
    try {
      await fileShare.persistPermission([{
        uri: uri, operationMode: fileShare.OperationMode.READ_MODE
      }]);
    } catch (err) {
      const e = err as BusinessError;
      // 201 means ohos.permission.FILE_ACCESS_PERSIST is missing from
      // module.json5. Not fatal for THIS session -- the grant is live until the
      // process dies -- so keep going and let the next launch surface it.
      log('persistPermission failed code=' + e.code + ' ' + e.message);
    }
  }

  /**
   * Re-arm every stored grant, and report which models no longer resolve.
   *
   * MUST run before the first model load of a session: a persisted grant is
   * dormant until activated, so without this a path opens fine in the session
   * that picked it and fails on every launch after -- with the file still sitting
   * there, which makes it look like the app forgot rather than lost access.
   *
   * Returns the ids whose file is gone. The caller decides what to do with them;
   * this does not edit the model list, because the list belongs to ModelStore and
   * two owners of one list is how entries start disappearing for no reason.
   */
  static async activate(models: ModelConfig[]): Promise<string[]> {
    const policies: fileShare.PolicyInfo[] = [];
    for (const m of models) {
      const uri: string | undefined = m.modelUri;
      if (uri !== undefined && uri.length > 0) {
        policies.push({ uri: uri, operationMode: fileShare.OperationMode.READ_MODE });
      }
    }
    if (policies.length === 0) {
      return [];
    }

    try {
      await fileShare.activatePermission(policies);
    } catch (err) {
      // Partial failure is normal (one moved file among several); the stat sweep
      // below decides what actually survived rather than trusting this verdict.
      const e = err as BusinessError;
      log('activatePermission code=' + e.code + ' ' + e.message);
    }

    const gone: string[] = [];
    for (const m of models) {
      const path: string | undefined = m.modelPath;
      if (path === undefined || path.length === 0) {
        gone.push(m.id);
        continue;
      }
      try {
        fs.statSync(path);
      } catch (err) {
        // Moving the file changes its path, which invalidates the reference.
        gone.push(m.id);
        log('on-device model unreachable: ' + m.id + ' at ' + path);
      }
    }
    return gone;
  }
}
