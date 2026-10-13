/**
 * Speech output's settings: which model files, which voice, how replies are read.
 *
 * What the desktop keeps in two files -- tts.json for the model paths, the
 * voice mode, the speaker and the seed, reader.json for the rest -- in one
 * preferences store here, because on this app one lane speaks and one page
 * sets it up. See examples/desktop/src/main/tts.ts and reader.ts for where each
 * default came from; the reasons are kept beside the values below.
 *
 * Read once into memory by init() and written through, so the reader can ask
 * synchronously while a reply streams.
 *
 * A file is a REFERENCE, never a copy, the same rule LocalModelStore keeps. A
 * file the user picked carries the URI its grant is keyed on; a file
 * downloaded here has none of its own and is reached through its folder's
 * grant, or through no grant at all when the folder is the app's own.
 */
import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import fs from '@ohos.file.fs';
import hilog from '@ohos.hilog';
import { LocalModelStore } from './LocalModelStore';
import { SpeechSlot } from './ModelFiles';

const STORE_NAME: string = 'speech';

/** A file the app reads: the path native opens, and the grant's URI ('' for none of its own). */
export interface FileRef {
  uri: string;
  path: string;
  name: string;
}

/** A folder downloads go into: its grant's URI ('' for the app's own storage) and path. */
export interface FolderRef {
  uri: string;
  path: string;
}

export interface SpeechSettings {
  /** The Base talker, which voice mode 'base' reads with. */
  talker: FileRef | null;
  /** The CustomVoice talker, which voice mode 'custom' reads with. */
  customTalker: FileRef | null;
  /** The 12 Hz audio tokenizer, which both modes need. */
  tokenizer: FileRef | null;
  /** 'base' clones a reference voice; 'custom' uses a speaker built into the model. */
  voiceMode: string;
  /** Which named speaker, on a custom_voice checkpoint. '' = the default below. */
  speaker: string;
  /** Sampling seed; see DEFAULT_SEED. */
  seed: number;
  /** Read every reply aloud as it is written. */
  enabled: boolean;
  /** The reference voice the user chose, a .wav or a .spk; null for a bundled one. */
  reference: FileRef | null;
  /** Which bundled voice, while `reference` is null. '' for the default. */
  bundled: string;
  /** How much text must have settled before a piece is sent. 0 = the lane's default. */
  minChars: number;
  /** The talker's sampling temperature, on Base. */
  temperature: number;
  /** How a CustomVoice talker is asked to read: '' | 'calm' | 'caring' | 'cheerful' | 'sorry'. */
  style: string;
  /** Where the last download went, offered first next time. null before the first. */
  downloadDir: FolderRef | null;
}

/**
 * A FIXED seed, where the ABI's own default draws a fresh one per call: two
 * runs with the seed unset were measured to come back as two different
 * recordings, and the test button and a reply did not sound like the same
 * person. 101 is the seed with rhythm, chosen by ear on the desktop.
 */
export const DEFAULT_SEED: number = 101;

/**
 * The talker's temperature when none has been chosen: 1.3, above the engine's
 * own 0.9 -- more varied pace and phrasing, preferred by ear on the desktop
 * with seed 101. Base only: CustomVoice reads at 0.9 whatever is set.
 */
export const DEFAULT_TEMPERATURE: number = 1.3;

function log(msg: string): void {
  hilog.info(0x0000, 'yoai', '%{public}s', msg);
}

function defaults(): SpeechSettings {
  return {
    talker: null, customTalker: null, tokenizer: null,
    voiceMode: 'base', speaker: '', seed: DEFAULT_SEED, enabled: true,
    reference: null, bundled: '', minChars: 0, temperature: DEFAULT_TEMPERATURE,
    style: '', downloadDir: null
  };
}

function parseFile(raw: string): FileRef | null {
  if (raw.length === 0) {
    return null;
  }
  try {
    const ref: FileRef = JSON.parse(raw) as FileRef;
    return typeof ref.path === 'string' && ref.path.length > 0 ? ref : null;
  } catch (err) {
    return null;
  }
}

function parseFolder(raw: string): FolderRef | null {
  if (raw.length === 0) {
    return null;
  }
  try {
    const ref: FolderRef = JSON.parse(raw) as FolderRef;
    return typeof ref.path === 'string' && ref.path.length > 0 ? ref : null;
  } catch (err) {
    return null;
  }
}

function reachable(path: string): boolean {
  try {
    fs.statSync(path);
    return true;
  } catch (err) {
    return false;
  }
}

export class SpeechStore {
  private static store: preferences.Preferences | null = null;
  private static current: SpeechSettings = defaults();
  private static loaded: boolean = false;

  private static async prefs(ctx: common.Context): Promise<preferences.Preferences> {
    if (SpeechStore.store === null) {
      SpeechStore.store = await preferences.getPreferences(ctx, STORE_NAME);
    }
    return SpeechStore.store;
  }

  /**
   * Read the store into memory and re-arm every grant it depends on, dropping
   * a file that has moved or gone.
   *
   * MUST run before the first load of a session, for the reason
   * LocalModelStore.activate gives: a persisted grant is dormant until
   * activated, and a path that opened in the session that picked it fails on
   * every launch after. A file that no longer resolves is forgotten rather than
   * kept -- a reference that fails on every launch is worse than none, and the
   * settings page then offers to pick or download it again.
   */
  static async init(ctx: common.Context): Promise<void> {
    if (SpeechStore.loaded) {
      return;
    }
    const p: preferences.Preferences = await SpeechStore.prefs(ctx);
    const s: SpeechSettings = defaults();
    s.talker = parseFile(await p.get('talker', '') as string);
    s.customTalker = parseFile(await p.get('customTalker', '') as string);
    s.tokenizer = parseFile(await p.get('tokenizer', '') as string);
    s.voiceMode = (await p.get('voiceMode', 'base') as string) === 'custom' ? 'custom' : 'base';
    s.speaker = await p.get('speaker', '') as string;
    s.seed = await p.get('seed', DEFAULT_SEED) as number;
    s.enabled = await p.get('enabled', true) as boolean;
    s.reference = parseFile(await p.get('reference', '') as string);
    s.bundled = await p.get('bundled', '') as string;
    s.minChars = await p.get('minChars', 0) as number;
    s.temperature = await p.get('temperature', DEFAULT_TEMPERATURE) as number;
    s.style = await p.get('style', '') as string;
    s.downloadDir = parseFolder(await p.get('downloadDir', '') as string);

    // The folder first: the files downloaded into it have no grant of their own.
    const dir: FolderRef | null = s.downloadDir;
    if (dir !== null && dir.uri.length > 0) {
      await LocalModelStore.activateOne(dir.uri, dir.path, true);
    }
    SpeechStore.current = s;
    SpeechStore.loaded = true;

    const slots: SpeechSlot[] = ['talker', 'customTalker', 'tokenizer'];
    for (const slot of slots) {
      const ref: FileRef | null = SpeechStore.file(slot);
      if (ref !== null && !(await SpeechStore.arm(ref))) {
        log('speech model ' + ref.name + ' is no longer at ' + ref.path);
        await SpeechStore.setFile(ctx, slot, null);
      }
    }
    const ref: FileRef | null = s.reference;
    if (ref !== null && !(await SpeechStore.arm(ref))) {
      log('reference voice ' + ref.name + ' is no longer at ' + ref.path);
      await SpeechStore.setReference(ctx, null);
    }
  }

  /** Re-activate a file's own grant, if it has one, and check it is there. */
  private static async arm(ref: FileRef): Promise<boolean> {
    if (ref.uri.length > 0) {
      return LocalModelStore.activateOne(ref.uri, ref.path);
    }
    return reachable(ref.path);
  }

  /** The settings as they stand. A copy: change them through the setters. */
  static get(): SpeechSettings {
    const s: SpeechSettings = SpeechStore.current;
    return {
      talker: s.talker, customTalker: s.customTalker, tokenizer: s.tokenizer,
      voiceMode: s.voiceMode, speaker: s.speaker, seed: s.seed, enabled: s.enabled,
      reference: s.reference, bundled: s.bundled, minChars: s.minChars,
      temperature: s.temperature, style: s.style, downloadDir: s.downloadDir
    };
  }

  static file(slot: SpeechSlot): FileRef | null {
    const s: SpeechSettings = SpeechStore.current;
    return slot === 'talker' ? s.talker : slot === 'customTalker' ? s.customTalker : s.tokenizer;
  }

  /** The talker slot the current voice mode loads. */
  static talkerSlot(): SpeechSlot {
    return SpeechStore.current.voiceMode === 'custom' ? 'customTalker' : 'talker';
  }

  /** The two files the current voice mode loads, talker then tokenizer; '' for either not there. */
  static paths(): string[] {
    const talker: FileRef | null = SpeechStore.file(SpeechStore.talkerSlot());
    const tokenizer: FileRef | null = SpeechStore.current.tokenizer;
    return [talker === null ? '' : talker.path, tokenizer === null ? '' : tokenizer.path];
  }

  /** The slots with no file: never picked, or picked and since gone. */
  static emptySlots(): SpeechSlot[] {
    const out: SpeechSlot[] = [];
    const slots: SpeechSlot[] = ['talker', 'customTalker', 'tokenizer'];
    for (const slot of slots) {
      if (SpeechStore.file(slot) === null) {
        out.push(slot);
      }
    }
    return out;
  }

  /** Of those, the ones the current voice mode loads: its talker, then the tokenizer. */
  static missingSlots(): SpeechSlot[] {
    const empty: SpeechSlot[] = SpeechStore.emptySlots();
    const want: SpeechSlot[] = [SpeechStore.talkerSlot(), 'tokenizer'];
    const out: SpeechSlot[] = [];
    for (const slot of want) {
      if (empty.includes(slot)) {
        out.push(slot);
      }
    }
    return out;
  }

  //----------------------------------------------------------------------------
  // Setters. Each writes memory first and then the store.
  //----------------------------------------------------------------------------

  private static async put(ctx: common.Context, key: string,
                           value: string | number | boolean): Promise<void> {
    const p: preferences.Preferences = await SpeechStore.prefs(ctx);
    await p.put(key, value);
    await p.flush();
  }

  static async setFile(ctx: common.Context, slot: SpeechSlot, ref: FileRef | null): Promise<void> {
    if (slot === 'talker') {
      SpeechStore.current.talker = ref;
    } else if (slot === 'customTalker') {
      SpeechStore.current.customTalker = ref;
    } else {
      SpeechStore.current.tokenizer = ref;
    }
    await SpeechStore.put(ctx, slot, ref === null ? '' : JSON.stringify(ref));
  }

  static async setVoiceMode(ctx: common.Context, mode: string): Promise<void> {
    SpeechStore.current.voiceMode = mode === 'custom' ? 'custom' : 'base';
    await SpeechStore.put(ctx, 'voiceMode', SpeechStore.current.voiceMode);
  }

  static async setSpeaker(ctx: common.Context, name: string): Promise<void> {
    SpeechStore.current.speaker = name;
    await SpeechStore.put(ctx, 'speaker', name);
  }

  /** A seed that is not a number is the default. */
  static async setSeed(ctx: common.Context, seed: number): Promise<void> {
    SpeechStore.current.seed = Number.isFinite(seed) ? Math.trunc(seed) : DEFAULT_SEED;
    await SpeechStore.put(ctx, 'seed', SpeechStore.current.seed);
  }

  static async setEnabled(ctx: common.Context, on: boolean): Promise<void> {
    SpeechStore.current.enabled = on;
    await SpeechStore.put(ctx, 'enabled', on);
  }

  static async setReference(ctx: common.Context, ref: FileRef | null): Promise<void> {
    SpeechStore.current.reference = ref;
    await SpeechStore.put(ctx, 'reference', ref === null ? '' : JSON.stringify(ref));
  }

  static async setBundled(ctx: common.Context, name: string): Promise<void> {
    SpeechStore.current.bundled = name;
    await SpeechStore.put(ctx, 'bundled', name);
  }

  /** 0, or anything not a positive number, is the lane's default. */
  static async setMinChars(ctx: common.Context, n: number): Promise<void> {
    SpeechStore.current.minChars = Number.isFinite(n) && n > 0 ? Math.trunc(n) : 0;
    await SpeechStore.put(ctx, 'minChars', SpeechStore.current.minChars);
  }

  /** Clamped to 0..2; anything not a number is the default. */
  static async setTemperature(ctx: common.Context, t: number): Promise<void> {
    SpeechStore.current.temperature =
      Number.isFinite(t) && t >= 0 ? Math.min(t, 2) : DEFAULT_TEMPERATURE;
    await SpeechStore.put(ctx, 'temperature', SpeechStore.current.temperature);
  }

  static async setStyle(ctx: common.Context, style: string): Promise<void> {
    const known: string[] = ['', 'calm', 'caring', 'cheerful', 'sorry'];
    SpeechStore.current.style = known.includes(style) ? style : '';
    await SpeechStore.put(ctx, 'style', SpeechStore.current.style);
  }

  static async setDownloadDir(ctx: common.Context, dir: FolderRef): Promise<void> {
    SpeechStore.current.downloadDir = dir;
    await SpeechStore.put(ctx, 'downloadDir', JSON.stringify(dir));
  }
}
