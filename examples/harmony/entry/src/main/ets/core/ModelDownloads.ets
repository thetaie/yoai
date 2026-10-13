/**
 * The two downloads the settings pages offer -- the on-device language model
 * and the speech models -- and where they go.
 *
 * Module-wide rather than owned by a page, so a download carries on while its
 * page is closed and the page follows it again when reopened; the desktop runs
 * them in its main process for the same reason.
 *
 * WHERE is the one thing that differs from the desktop, which always asks for
 * a folder. A 2-in-1 device does too. A phone cannot: the picker's folder mode
 * is for 2-in-1 devices only. So a phone downloads into the app's own storage,
 * filesDir/models -- out of the user's sight, and deleted with the app.
 */
import { common } from '@kit.AbilityKit';
import fs from '@ohos.file.fs';
import { KIND_LOCAL, LOCAL_VENDOR_ID, ModelConfig, modelId } from '../model/Provider';
import { Downloader } from './Download';
import { LocalModelStore, PickedFolder } from './LocalModelStore';
import { LLM_OFFER, SpeechSlot, speechFile } from './ModelFiles';
import { ModelStore } from './ModelStore';
import { Reader } from './Reader';
import { FolderRef, SpeechStore } from './SpeechStore';

export class ModelDownloads {
  static readonly llm: Downloader = new Downloader('language model');
  static readonly speech: Downloader = new Downloader('speech model');

  /** Where downloads go when the device cannot pick a folder. */
  static appDir(ctx: common.Context): string {
    return ctx.filesDir + '/models';
  }

  /**
   * The folder to download into: one the user picks on a 2-in-1 device, offered
   * again next time, or the app's own on a phone. undefined when the user
   * dismissed the picker.
   */
  static async chooseDir(ctx: common.UIAbilityContext): Promise<FolderRef | undefined> {
    if (LocalModelStore.folderPickable()) {
      const picked: PickedFolder | undefined = await LocalModelStore.pickFolder(ctx);
      if (picked === undefined) {
        return undefined;
      }
      const dir: FolderRef = { uri: picked.uri, path: picked.path };
      await SpeechStore.setDownloadDir(ctx, dir);
      return dir;
    }
    const path: string = ModelDownloads.appDir(ctx);
    fs.mkdirSync(path, true);
    return { uri: '', path: path };
  }

  /**
   * Download the offered model and add it, selected when nothing else is, then
   * read it as switching to it would. False when the folder picker was
   * dismissed, the engine is missing, or a download is already running.
   */
  static async downloadLlm(ctx: common.UIAbilityContext): Promise<boolean> {
    if (!LocalModelStore.available() || ModelDownloads.llm.status().running) {
      return false;
    }
    const dir: FolderRef | undefined = await ModelDownloads.chooseDir(ctx);
    if (dir === undefined) {
      return false;
    }
    ModelDownloads.llm.run(dir.path, [LLM_OFFER], async () => {
      const name: string = LLM_OFFER.name;
      const cfg: ModelConfig = {
        id: modelId(LOCAL_VENDOR_ID, name),
        vendorId: LOCAL_VENDOR_ID,
        baseUrl: '',
        model: name,
        kind: KIND_LOCAL,
        // No grant of its own: it is reached through its folder's, which
        // SpeechStore re-arms at startup, or needs none in the app's storage.
        modelPath: dir.path + '/' + name
      };
      await ModelStore.add(ctx, cfg);
      if ((await ModelStore.activeId(ctx)) === cfg.id) {
        ModelStore.autoPreload(ctx);
      }
    });
    return true;
  }

  /**
   * Download whichever of `slots` is not already in the folder, store every
   * slot's path, and load the voice again. False when there is nothing to
   * fetch, the picker was dismissed, or a download is already running.
   */
  static async downloadSpeech(ctx: common.UIAbilityContext, slots: SpeechSlot[]): Promise<boolean> {
    if (slots.length === 0 || ModelDownloads.speech.status().running) {
      return false;
    }
    const dir: FolderRef | undefined = await ModelDownloads.chooseDir(ctx);
    if (dir === undefined) {
      return false;
    }
    ModelDownloads.speech.run(dir.path, slots.map((s: SpeechSlot) => speechFile(s)), async () => {
      for (const slot of slots) {
        const name: string = speechFile(slot).name;
        await SpeechStore.setFile(ctx, slot, { uri: '', path: dir.path + '/' + name, name: name });
      }
      Reader.reload();
    });
    return true;
  }
}
