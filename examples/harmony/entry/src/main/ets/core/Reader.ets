/**
 * Reading a reply aloud while the model is still writing it -- the one lane
 * this app speaks through: a reply as it is written, a message's 朗读, the
 * settings test.
 *
 * A port of two desktop files, the main process's reader.ts (the engine, its
 * settings, the voice it is anchored to) and the renderer's reader.ts (the
 * stream and the player), which on this platform are one process and one file.
 * The constants are the desktop's and so are their reasons, given beside each.
 *
 * Two rules come from this platform, not the desktop. Every native call here
 * runs on the ArkTS thread, and a load or a voice extraction holds the engine's
 * lock for seconds -- so nothing asks the engine anything while one runs: what
 * the screen shows is a mirror kept here, filled in when a load finishes. And
 * the settings are handed to the engine when it loads and before each stream,
 * never in the middle of one.
 */
import { common } from '@kit.AbilityKit';
import fs from '@ohos.file.fs';
import hilog from '@ohos.hilog';
import yoai from 'libyoai.so';
import { LocalModelStore } from './LocalModelStore';
import { player } from './SpeechPlayer';
import { FileRef, SpeechSettings, SpeechStore } from './SpeechStore';

/** AppStorage key, bumped whenever status() would answer differently. */
export const READER_REV: string = 'readerRev';

/**
 * The voice a Base checkpoint is anchored to when the user has not picked one:
 * a directory under speaker/ at the repository root, copied into this app's
 * rawfile/speaker/ when it is built (entry/hvigorfile.ts), holding one .spk per
 * model size.
 *
 * A speaker embedding WITHOUT a recording. Base checkpoints only, and the reason
 * it is not optional: every piece is a separate generation, and with nothing
 * acoustic in common they drift -- one sentence in sixteen came out five
 * semitones low, heard as the voice changing mid-reply.
 */
const SPEAKER: string = '01_female';

/** The speaker a CustomVoice talker uses when none has been picked. */
const DEFAULT_SPEAKER: string = 'serena';

/**
 * The cut when nothing has been chosen, finer than the core's 30: on a slow
 * writer, how much text must be written before the first piece is the whole of
 * the wait. A compromise against a slow writer, not a better answer than 30.
 */
const DEFAULT_MIN_CHARS: number = 12;
/** And past this a piece is cut at the last comma, with no full stop to cut at. */
const DEFAULT_MAX_CHARS: number = 40;
/** The pieces after the first, which can wait for more text while the one before plays. */
const LATER_MIN_CHARS: number = 30;
const LATER_MAX_CHARS: number = 80;
/** The first piece may end at a full stop from 6 characters on: "我知道苏州河。" goes at once. */
const FIRST_STOP_CHARS: number = 6;

/**
 * Each piece's text one token per frame, and two frames past the first stop
 * code so an ending decays rather than stopping mid vowel. Need
 * qwentts-streaming-text.patch, which third/sync.py applies.
 */
const STREAMING_TEXT: boolean = true;
const EOS_HOLD_FRAMES: number = 2;

/**
 * A reply read as ONE generation that takes the text as it is written: no
 * seams. The writer has to stay ahead of the talker's 12.5 tokens a second, and
 * on a phone the talker is itself slower than real time, so the player gathers
 * the reply and plays it once it has it (SpeechPlayer, REALTIME_MARGIN).
 */
const WHOLE_REPLY: boolean = true;

/**
 * Seconds of audio in hand before the first word, for this lane: it waits for
 * the language model between pieces as well as for the engine, so it needs a
 * deeper cushion than the player's default. 1.2 was measured on the desktop.
 */
const PRIME_SECONDS: number = 1.2;

/** The instruction each CustomVoice style sends; few and short on purpose. */
function styleText(style: string): string {
  if (style === 'calm') {
    return '语气平静。';
  }
  if (style === 'caring') {
    return '语气关切，温柔。';
  }
  if (style === 'cheerful') {
    return '语气轻快，开心。';
  }
  if (style === 'sorry') {
    return '语气抱歉，诚恳。';
  }
  return '';
}

/** What the settings page shows, and why the reader cannot read when it cannot. */
export interface ReaderStatus {
  available: boolean;
  /** Reading the two GGUFs, which takes seconds. */
  loading: boolean;
  /** Extracting a recording's embedding, which takes seconds too. */
  extracting: boolean;
  loaded: boolean;
  /** 'voice_design', 'base' or 'custom_voice'; '' before a load. */
  modelType: string;
  /** The named speakers of a CustomVoice talker; [] on any other. */
  speakers: string[];
  /** The voice every piece is anchored to, as chosen; '' when none. */
  anchoredTo: string;
  /** What went wrong with the last load or voice, '' when nothing did. */
  note: string;
}

interface Pending {
  resolve: (stats: string) => void;
  reject: (e: Error) => void;
}

function log(msg: string): void {
  hilog.info(0x0000, 'yoai', '%{public}s', msg);
}

function reachable(path: string): boolean {
  try {
    fs.statSync(path);
    return true;
  } catch (err) {
    return false;
  }
}

/** A short stable key for a string: two FNV-1a passes, 16 hex characters. */
function keyOf(text: string): string {
  let a: number = 0x811c9dc5;
  let b: number = 0x01000193;
  for (let i = 0; i < text.length; i++) {
    const c: number = text.charCodeAt(i);
    a = Math.imul(a ^ c, 0x01000193) >>> 0;
    b = Math.imul(b ^ c, 0x811c9dc5) >>> 0;
  }
  return ('0000000' + a.toString(16)).slice(-8) + ('0000000' + b.toString(16)).slice(-8);
}

export class Reader {
  private static ctx: common.Context | null = null;
  private static availability: number = -1;

  // The mirror. Filled in when a load finishes, cleared by an unload.
  private static loading: boolean = false;
  private static loadPromise: Promise<string> | null = null;
  private static extracting: boolean = false;
  private static loaded: boolean = false;
  private static modelType: string = '';
  private static speakers: string[] = [];
  private static hidden: number = 0;
  private static anchoredTo: string = '';
  private static note: string = '';

  // The stream.
  private static open: boolean = false;
  /**
   * Which stream is open, so a caller acts only on its own: a reply still
   * being written must not feed, finish or cancel the stream a message's 朗读
   * opened after it.
   */
  private static streamId: number = 0;
  private static closing: Promise<string> | null = null;
  private static pending: Pending | null = null;
  /** Set by cancel(): audio still in flight for the old stream is not played. */
  private static dropping: boolean = false;
  private static openedAt: number = 0;
  private static firstChunk: boolean = false;
  private static lastSentence: number = -1;

  /**
   * Called when whether replies are read may have changed -- a load, an
   * unload -- so the on-device model's prompt can be warmed again. Set by the
   * chat page; kept a hook so this file does not import ModelStore, which
   * imports it.
   */
  static onVoiceChanged: (() => void) | null = null;

  /** Remember the context: the voice files are written under its filesDir. */
  static init(ctx: common.Context): void {
    Reader.ctx = ctx;
  }

  static available(): boolean {
    if (Reader.availability < 0) {
      Reader.availability = yoai.nativeReaderAvailable() ? 1 : 0;
    }
    return Reader.availability === 1;
  }

  static status(): ReaderStatus {
    return {
      available: Reader.available(),
      loading: Reader.loading,
      extracting: Reader.extracting,
      loaded: Reader.loaded,
      modelType: Reader.modelType,
      speakers: Reader.speakers.slice(),
      anchoredTo: Reader.anchoredTo,
      note: Reader.note
    };
  }

  /** Whether the engine is up and free -- what 朗读 and the settings test need. */
  static canSpeak(): boolean {
    return Reader.available() && Reader.loaded && !Reader.loading && !Reader.extracting;
  }

  /**
   * Whether a reply should be read as it is written. Asked fresh at every turn:
   * the voice loads in the background, and a value captured earlier says "not
   * loaded" for the rest of the session.
   */
  static ready(): boolean {
    return Reader.canSpeak() && SpeechStore.get().enabled;
  }

  private static publish(): void {
    const rev: number | undefined = AppStorage.get<number>(READER_REV);
    AppStorage.setOrCreate(READER_REV, (rev === undefined ? 0 : rev) + 1);
  }

  private static changed(): void {
    Reader.publish();
    const hook: (() => void) | null = Reader.onVoiceChanged;
    if (hook !== null) {
      hook();
    }
  }

  //----------------------------------------------------------------------------
  // Loading
  //----------------------------------------------------------------------------

  /**
   * Read the two GGUFs. Resolves to '' on success, or to the reason.
   *
   * The speech lane (the /tts page) is unloaded first: two engines over the
   * same 1.5 GB is the one configuration nobody wants.
   */
  static load(): Promise<string> {
    if (!Reader.available()) {
      return Promise.resolve('this build has no speech engine -- build.py qtts turns it on');
    }
    if (Reader.loadPromise !== null) {
      return Reader.loadPromise;
    }
    if (Reader.loaded) {
      return Promise.resolve('');
    }
    const paths: string[] = SpeechStore.paths();
    if (paths[0].length === 0 || paths[1].length === 0) {
      return Promise.resolve('pick the talker and the tokenizer first');
    }
    if (!reachable(paths[0]) || !reachable(paths[1])) {
      return Promise.resolve('one of the model files is missing');
    }
    if (yoai.nativeTtsLoaded()) {
      yoai.nativeTtsUnload();
    }

    Reader.loading = true;
    Reader.publish();
    Reader.loadPromise = Reader.readAndLoad(paths[0], paths[1]);
    return Reader.loadPromise;
  }

  /**
   * The two files read once first, then the load: mapped straight from user
   * storage, a model froze the ArkTS thread long enough for the system to kill
   * the app (see nativePrefetch).
   */
  private static async readAndLoad(talker: string, tokenizer: string): Promise<string> {
    const started: number = Date.now();
    await LocalModelStore.prefetch(talker);
    await LocalModelStore.prefetch(tokenizer);
    return new Promise<string>((resolve: (why: string) => void) => {
      // One engine, not several: a second voice measured slightly SLOWER on the
      // desktop and costs another copy of the weights.
      yoai.nativeReaderLoad(talker, tokenizer, 1, (type: string, content: string) => {
        Reader.loading = false;
        if (type !== 'end') {
          Reader.loadPromise = null;
          Reader.publish();
          resolve(content.length > 0 ? content : 'the model would not load');
          return;
        }
        log('reader loaded in ' + (Date.now() - started) + ' ms');
        // The engine's lock is free now; this is the one time it is asked.
        Reader.loaded = true;
        Reader.modelType = yoai.nativeReaderModelType();
        Reader.hidden = yoai.nativeReaderTalkerHiddenSize();
        Reader.speakers = Reader.parseSpeakers(yoai.nativeReaderSpeakers());
        Reader.applySettings();
        // The reference voice, on a Base checkpoint only. Not fatal: the lane
        // reads without an anchor, it just drifts more.
        Reader.applyReference().then((why: string) => {
          Reader.loadPromise = null;
          Reader.publish();
          resolve(why.length > 0 ? 'loaded, but the reference voice failed: ' + why : '');
        });
      });
    });
  }

  /** Let go of the weights, after any stream and any load in flight. */
  static async unload(): Promise<void> {
    if (Reader.loadPromise !== null) {
      await Reader.loadPromise;
    }
    await Reader.cancel();
    if (!Reader.loaded) {
      return;
    }
    yoai.nativeReaderUnload();
    Reader.loaded = false;
    Reader.modelType = '';
    Reader.speakers = [];
    Reader.hidden = 0;
    Reader.anchoredTo = '';
    Reader.changed();
  }

  /**
   * Load the voice without being asked, whenever both files of the current
   * mode are there. Having to press a load button every launch is a step that
   * exists only to be forgotten. Base or CustomVoice; VoiceDesign is a way to
   * MAKE a reference voice, not to talk, and is unloaded again and said so.
   */
  static async autoLoadVoice(): Promise<string> {
    const paths: string[] = SpeechStore.paths();
    if (!Reader.available() || paths[0].length === 0 || paths[1].length === 0) {
      return '';
    }
    if (Reader.loaded || Reader.loadPromise !== null) {
      return '';
    }
    Reader.note = '';
    let reason: string = await Reader.load();
    if (Reader.loaded && Reader.modelType === 'voice_design') {
      reason = 'needs a Base or CustomVoice talker; this one is voice_design';
      await Reader.unload();
    }
    Reader.note = reason;
    Reader.changed();
    if (reason.length > 0) {
      hilog.error(0x0000, 'yoai', 'the voice would not load: %{public}s', reason);
    }
    return reason;
  }

  /** Load again from the stored files: one was just replaced, or the mode changed. */
  static async reload(): Promise<string> {
    await Reader.unload();
    return Reader.autoLoadVoice();
  }

  private static parseSpeakers(raw: string): string[] {
    try {
      return JSON.parse(raw) as string[];
    } catch (err) {
      return [];
    }
  }

  /**
   * Everything the engine is told that is not a file: from the store, at load
   * and before each stream. Never while a stream is open or a load or an
   * extraction holds the engine's lock.
   */
  private static applySettings(): void {
    if (!Reader.loaded || Reader.loading || Reader.extracting || Reader.open) {
      return;
    }
    const s: SpeechSettings = SpeechStore.get();
    const custom: boolean = Reader.modelType === 'custom_voice';
    yoai.nativeReaderSetSeed(s.seed);
    // CustomVoice reads at the engine's own 0.9: at 1.3 it was heard to read
    // less naturally than Base, and it has no speaker vector to hold it.
    yoai.nativeReaderSetTemperature(custom ? -1 : s.temperature);
    yoai.nativeReaderSetSplit(s.minChars > 0 ? s.minChars : DEFAULT_MIN_CHARS, DEFAULT_MAX_CHARS, 0);
    yoai.nativeReaderSetLaterSplit(LATER_MIN_CHARS, LATER_MAX_CHARS);
    yoai.nativeReaderSetFirstStopChars(FIRST_STOP_CHARS);
    yoai.nativeReaderSetStreamingText(STREAMING_TEXT);
    yoai.nativeReaderSetEosHold(EOS_HOLD_FRAMES);
    yoai.nativeReaderSetWholeReply(WHOLE_REPLY);
    // Without it a comma is only a fallback at max_chars, and a reply whose one
    // full stop is at the end cannot be started until it is all written.
    yoai.nativeReaderSetClauseCuts(true);
    if (!custom) {
      // The engine refuses both on any other checkpoint, and would keep one
      // left over from an earlier model.
      yoai.nativeReaderSetSpeaker('');
      yoai.nativeReaderSetVoice('');
      return;
    }
    const names: string[] = Reader.speakers;
    const speaker: string = names.includes(s.speaker) ? s.speaker
      : names.includes(DEFAULT_SPEAKER) ? DEFAULT_SPEAKER
      : names.length > 0 ? names[0] : '';
    yoai.nativeReaderSetSpeaker(speaker);
    yoai.nativeReaderSetVoice(styleText(s.style));
  }

  //----------------------------------------------------------------------------
  // The voice a Base checkpoint is anchored to
  //----------------------------------------------------------------------------

  /** The voices that come with the app: each directory under rawfile/speaker holding a .spk. */
  static bundledVoices(): string[] {
    const ctx: common.Context | null = Reader.ctx;
    if (ctx === null) {
      return [];
    }
    try {
      const out: string[] = [];
      for (const name of ctx.resourceManager.getRawFileListSync('speaker')) {
        const files: string[] = ctx.resourceManager.getRawFileListSync('speaker/' + name);
        if (files.some((f: string) => f.endsWith('.spk'))) {
          out.push(name);
        }
      }
      out.sort();
      return out;
    } catch (err) {
      return [];
    }
  }

  /** The bundled voice in use while no file is chosen: the stored one if it is still there. */
  static bundledName(): string {
    const stored: string = SpeechStore.get().bundled;
    return stored.length > 0 && Reader.bundledVoices().includes(stored) ? stored : SPEAKER;
  }

  /**
   * The bundled voice `name` as files native can open: copied out of the app's
   * resources into filesDir/speaker/<name>/ once, and again only when a size
   * differs. Returns that directory, or '' when it could not be written.
   */
  private static bundledDir(name: string): string {
    const ctx: common.Context | null = Reader.ctx;
    if (ctx === null) {
      return '';
    }
    const dir: string = ctx.filesDir + '/speaker/' + name;
    try {
      fs.mkdirSync(dir, true);
      for (const f of ctx.resourceManager.getRawFileListSync('speaker/' + name)) {
        if (!f.endsWith('.spk')) {
          continue;
        }
        const bytes: Uint8Array = ctx.resourceManager.getRawFileContentSync('speaker/' + name + '/' + f);
        const path: string = dir + '/' + f;
        let size: number = -1;
        try {
          size = fs.statSync(path).size;
        } catch (err) {
          size = -1;
        }
        if (size !== bytes.length) {
          const out: fs.File = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
          fs.writeSync(out.fd, bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.length));
          fs.closeSync(out);
        }
      }
      return dir;
    } catch (err) {
      log('the bundled voice ' + name + ' could not be written: ' + JSON.stringify(err));
      return '';
    }
  }

  /**
   * The .spk in `dir` for a talker `hidden` wide, or ''. Chosen by byte size,
   * not by name: a .spk is `hidden` f32 values and nothing else, so 4 x hidden
   * bytes is the file that fits, and a renamed file still works.
   */
  private static speakerFile(dir: string, hidden: number): string {
    if (hidden <= 0 || dir.length === 0) {
      return '';
    }
    try {
      for (const name of fs.listFileSync(dir)) {
        const path: string = dir + '/' + name;
        if (name.endsWith('.spk') && fs.statSync(path).size === hidden * 4) {
          return path;
        }
      }
    } catch (err) {
      return '';
    }
    return '';
  }

  /**
   * Where a recording's embedding is kept once extracted, for a talker `hidden`
   * wide. Extracting runs the model's speaker encoder, seconds, and the vector
   * is all that is used afterwards -- so it is paid once per recording and model
   * size. Keyed by path, size and modification time, so an edited recording is
   * extracted again.
   */
  private static cacheFile(wav: string, hidden: number): string {
    const ctx: common.Context | null = Reader.ctx;
    if (ctx === null) {
      return '';
    }
    const st: fs.Stat = fs.statSync(wav);
    const dir: string = ctx.filesDir + '/speaker-cache';
    fs.mkdirSync(dir, true);
    return dir + '/' + keyOf(wav + '\n' + st.size + '\n' + st.mtime) + '-' + hidden + '.spk';
  }

  /** Load a .spk; '' on success, else why not. */
  private static anchor(spk: string, shown: string): string {
    const why: string = yoai.nativeReaderLoadReferenceEmbedding(spk);
    if (why.length > 0) {
      return why;
    }
    Reader.anchoredTo = shown;
    return '';
  }

  /**
   * Anchor the loaded engine to the chosen voice. Resolves to '' when it is
   * anchored, or when there is nothing loaded to anchor yet -- load() calls
   * this again -- and to the reason otherwise.
   */
  private static applyReference(): Promise<string> {
    Reader.anchoredTo = '';
    if (!Reader.loaded || Reader.modelType !== 'base' || Reader.open) {
      return Promise.resolve('');
    }
    const chosen: FileRef | null = SpeechStore.get().reference;

    if (chosen === null) {
      const name: string = Reader.bundledName();
      const spk: string = Reader.speakerFile(Reader.bundledDir(name), Reader.hidden);
      if (spk.length === 0) {
        return Promise.resolve('the voice ' + name + ' has no .spk for a talker ' + Reader.hidden + ' wide');
      }
      return Promise.resolve(Reader.anchor(spk, name));
    }
    if (!reachable(chosen.path)) {
      return Promise.resolve('no file at ' + chosen.path);
    }
    // An embedding is loaded as it is; the core checks its length.
    if (!chosen.path.toLowerCase().endsWith('.wav')) {
      return Promise.resolve(Reader.anchor(chosen.path, chosen.name));
    }

    // A recording: its embedding from the cache when it has been extracted for
    // this model size before, and extracted now otherwise.
    const cached: string = Reader.cacheFile(chosen.path, Reader.hidden);
    if (cached.length > 0 && reachable(cached) && Reader.anchor(cached, chosen.name).length === 0) {
      return Promise.resolve('');
    }
    Reader.extracting = true;
    Reader.publish();
    return new Promise<string>((resolve: (why: string) => void) => {
      yoai.nativeReaderSetReferenceVoice(chosen.path, (type: string, content: string) => {
        Reader.extracting = false;
        if (type !== 'end') {
          Reader.publish();
          resolve(content.length > 0 ? content : 'the recording could not be used');
          return;
        }
        Reader.anchoredTo = chosen.name;
        if (cached.length > 0) {
          const why: string = yoai.nativeReaderSaveReferenceEmbedding(cached);
          if (why.length > 0) {
            // Anchored either way; only the next launch pays again.
            log('the reference voice was not cached: ' + why);
          }
        }
        Reader.publish();
        resolve('');
      });
    });
  }

  /**
   * Use `ref` -- a .wav or a .spk, or null for the bundled voice -- from now on,
   * and at once if the engine is loaded. Resolves to '' or the reason it failed.
   */
  static async setReference(ref: FileRef | null): Promise<string> {
    const ctx: common.Context | null = Reader.ctx;
    if (ctx !== null) {
      await SpeechStore.setReference(ctx, ref);
    }
    if (Reader.loading || Reader.extracting) {
      return '';   // the load in flight anchors to it when it gets there
    }
    await Reader.cancel();
    const why: string = await Reader.applyReference();
    Reader.note = why.length > 0 ? 'the reference voice failed: ' + why : '';
    Reader.publish();
    return why;
  }

  /** Use the bundled voice `name` in place of any file chosen. */
  static async useBundled(name: string): Promise<string> {
    if (!Reader.bundledVoices().includes(name)) {
      return 'no voice named ' + name;
    }
    const ctx: common.Context | null = Reader.ctx;
    if (ctx !== null) {
      await SpeechStore.setBundled(ctx, name);
    }
    return Reader.setReference(null);
  }

  //----------------------------------------------------------------------------
  // The stream
  //----------------------------------------------------------------------------

  /**
   * Open a stream, cancelling the one that is open. Resolves to the stream's id
   * for feed(), finish() and cancel(), or to 0 when the engine is not loaded or
   * is busy loading or extracting.
   */
  static async begin(): Promise<number> {
    if (Reader.open) {
      await Reader.cancel();
    }
    if (!Reader.canSpeak()) {
      return 0;
    }
    Reader.dropping = false;
    player.stop();
    player.prime(PRIME_SECONDS);
    Reader.applySettings();
    Reader.openedAt = Date.now();
    Reader.firstChunk = true;
    Reader.lastSentence = -1;

    const ok: boolean = yoai.nativeReaderBegin(
      (type: string, payload: ArrayBuffer | string, sentence: number) => {
        Reader.onEvent(type, payload, sentence);
      }) === 1;
    Reader.open = ok;
    if (!ok) {
      log('[reader] could not begin: ' + yoai.nativeReaderLastError());
      return 0;
    }
    // Something to stop from now, not from the first chunk: the stream's end
    // flushes the player, which lets this go again.
    player.expect();
    Reader.streamId++;
    return Reader.streamId;
  }

  private static onEvent(type: string, payload: ArrayBuffer | string, sentence: number): void {
    if (type === 'audio') {
      if (Reader.dropping || typeof payload === 'string') {
        return;
      }
      const pcm: Int16Array = new Int16Array(payload as ArrayBuffer);
      if (Reader.firstChunk) {
        Reader.firstChunk = false;
        log('[reader] first audio at the speaker: ' + (Date.now() - Reader.openedAt) + ' ms');
      }
      // The seams, which are what this lane trades for its latency: one line
      // per piece rather than per chunk.
      if (sentence !== Reader.lastSentence) {
        Reader.lastSentence = sentence;
        log('[reader] piece ' + sentence + ' starts at ' +
          ((Date.now() - Reader.openedAt) / 1000).toFixed(2) + ' s');
      }
      player.push(pcm);
      return;
    }

    player.flush();
    const p: Pending | null = Reader.pending;
    Reader.pending = null;
    Reader.open = false;
    Reader.closing = null;
    const text: string = typeof payload === 'string' ? payload as string : '';
    if (type === 'error') {
      hilog.error(0x0000, 'yoai', '[reader] %{public}s', text);
      if (p !== null) {
        p.reject(new Error(text.length > 0 ? text : 'reading failed'));
      }
      return;
    }
    log('[reader] done: ' + text);
    if (p !== null) {
      p.resolve(text);
    }
  }

  /**
   * Add a delta to stream `id`, exactly as the model wrote it, markdown and
   * all: the core turns it into what should be spoken, and can only do that on
   * the whole stream. Returns at once; ignored when `id` is no longer open.
   */
  static feed(id: number, delta: string): void {
    if (Reader.open && id === Reader.streamId && delta.length > 0) {
      yoai.nativeReaderFeed(delta);
    }
  }

  /**
   * No more text for stream `id` (0: whichever is open). Resolves with the
   * timings once the last sample has been generated -- not played: the player
   * still has what it was given. Resolves at once when `id` is no longer open.
   */
  static finish(id: number): Promise<string> {
    if (!Reader.open || (id !== 0 && id !== Reader.streamId)) {
      return Promise.resolve('');
    }
    const closing: Promise<string> | null = Reader.closing;
    if (closing !== null) {
      return closing;
    }
    const next: Promise<string> = new Promise<string>(
      (resolve: (stats: string) => void, reject: (e: Error) => void) => {
        Reader.pending = { resolve: resolve, reject: reject };
      });
    Reader.closing = next;
    if (yoai.nativeReaderFinish() !== 1) {
      Reader.onEvent('error', 'no stream was open', -1);
    }
    return next;
  }

  /**
   * Stop reading stream `id`, or whatever is speaking with 0: the engine
   * abandons the stream and what it already sent is not played. Resolves once
   * the stream is closed, so a begin() can follow. Does nothing for an `id`
   * that is no longer the open stream, whose speech is someone else's now.
   *
   * With 0 the player is stopped whether or not a stream is open: the engine is
   * often done while the speaker still has most of a reply to play, and that is
   * when a stop is most often asked for.
   */
  static async cancel(id: number = 0): Promise<void> {
    if (id !== 0 && (!Reader.open || id !== Reader.streamId)) {
      return;
    }
    player.stop();
    if (!Reader.open) {
      return;
    }
    Reader.dropping = true;
    yoai.nativeReaderCancel();
    try {
      await Reader.finish(0);
    } catch (err) {
      // Cancelled is how it was meant to end.
    }
  }
}
