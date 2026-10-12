/**
 * Reading a reply aloud while the model is still writing it.
 *
 * The speech lane in tts.ts speaks text that already exists, and that is the
 * whole of what is wrong with it for a conversation: the reply takes seconds to
 * write, so the wait is the language model's rather than the engine's, and no
 * amount of streaming inside the engine recovers it. This one is handed the
 * reply a token at a time and starts speaking the first sentence while the
 * model is still on the third.
 *
 * **It won, and it is now the only lane the desktop speaks through**: replies,
 * a message's 🔊, the settings screen's test. The speech lane in tts.ts is kept
 * for its settings -- the model paths and the seed live in tts.json -- and is no
 * longer loaded. The two are separate engines over the same GGUFs, so loading
 * this one still unloads that one, in case something else loaded it.
 *
 * What this lane adds is its own: the voice every piece is anchored to, which is
 * what holds it steady across pieces, and how coarsely to cut.
 */
import { app } from 'electron'
import { createHash } from 'node:crypto'
import { existsSync, mkdirSync, readdirSync, statSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'
import type { ReaderStatus, ReaderStyle } from '../shared/api'
import { core } from './core'
import { dataFile, readJson, writeJson } from './jsonfile'
import { paths as ttsPaths, voiceSettings } from './tts'

/**
 * The voice a Base checkpoint is anchored to when the user has not picked one:
 * a directory under speaker/ at the repository root, holding one .spk per model
 * size. Every other directory there is offered beside it; see bundledVoices().
 *
 * A speaker embedding WITHOUT a recording -- qwen.h's clone mode A. Base
 * checkpoints only, and the reason it is not optional: every piece is a separate
 * generation, and with nothing acoustic in common they drift. One sentence in
 * sixteen came out five semitones low, which is heard as the voice changing
 * mid-reply. Anchoring every piece to one embedding removed it on four seeds
 * out of four and cost nothing measurable in speed.
 *
 * A .spk rather than the recording itself, so no extraction runs at load: that
 * took about 4.5 s for a 17-second reference.
 */
const SPEAKER = '01_female'

interface Stored {
  /** Read every reply aloud as it is written, without being asked. */
  enabled: boolean

  /**
   * The reference voice the user picked: a recording (.wav) or an embedding
   * already extracted (.spk). '' for a bundled one, named by `bundled`.
   */
  reference: string

  /** Which voice under speaker/, while `reference` is ''. '' for SPEAKER. */
  bundled: string

  /**
   * How much text must have settled before a piece is sent.
   *
   * 0 means DEFAULT_MIN_CHARS below, not the core's own default -- see there.
   */
  minChars: number

  /** The talker's sampling temperature; see DEFAULT_TEMPERATURE. */
  temperature: number

  /** How a CustomVoice talker is asked to read; see STYLES. */
  style: ReaderStyle
}

/**
 * The styles a CustomVoice talker can be asked for, and the instruction each
 * one sends. Few and short on purpose: the instruction sits in the prompt
 * ahead of the text, and a handful of fixed ones cost almost nothing to add.
 *
 * Listened to on two replies and two seeds, read whole with the streaming text
 * layout: the four were told apart and the speaker stayed the same person.
 * 关切 read slowest every time; the others were heard rather than measured,
 * since their pitch and pace moved about as much between seeds as between
 * styles. 开心 drew a giggle at both ends on one seed of two, and stays --
 * with seed 101 it did not.
 */
const STYLES: Record<Exclude<ReaderStyle, ''>, string> = {
  calm: '语气平静。',
  caring: '语气关切，温柔。',
  cheerful: '语气轻快，开心。',
  sorry: '语气抱歉，诚恳。'
}

/** The speaker a CustomVoice talker uses when none has been picked. */
const DEFAULT_SPEAKER = 'serena'

/**
 * The talker's temperature when none has been chosen: 1.3, above the engine's
 * own 0.9.
 *
 * Higher reads with more varied pace and phrasing, and past some point it
 * breaks. Three passages on four seeds at 0.9, 1.1, 1.2 and 1.3: no take ran
 * long or short of its text (time per character within 15% of the median),
 * and 1.3 with seed 101 (DEFAULT_SEED in tts.ts) was preferred by ear. Its
 * one fault heard: a sentence starting a little louder than its neighbours,
 * inside one piece, which 0.9 to 1.2 did not show.
 *
 * Base only: CustomVoice ignores it, see talkerTemperature().
 */
const DEFAULT_TEMPERATURE = 1.3

/**
 * The temperature handed to the loaded talker: the stored one on Base, and on
 * CustomVoice always the engine's own 0.9, whatever is stored. -1 is the
 * engine's "back to the default".
 *
 * DEFAULT_TEMPERATURE was picked on Base, every piece anchored to a speaker
 * vector (tests/tts_sweep.cpp). CustomVoice has a speaker id and no vector, and
 * at 1.3 a listener heard it read less naturally than Base. The settings screen
 * greys the field out on CustomVoice to match.
 */
function talkerTemperature(s: Stored): number {
  return core().nativeReaderModelType() === 'custom_voice' ? -1 : s.temperature
}

/**
 * The cut this lane uses when nothing has been chosen, which is finer than the
 * core's own 30.
 *
 * The core's default was measured for how it SOUNDS: 30 characters a piece
 * beat 12 by ear, because a piece holding two or three sentences lets the model
 * plan a pace across them. That measurement stands and is not being overridden
 * lightly -- what it did not include is the writer.
 *
 * On this desktop the on-device model decodes at 3.7 to 9.1 tok/s, and the
 * slower figure is what it manages WHILE the speech engine is running: the two
 * compete for the same cores. At 3.7 tok/s, 36 characters took 6.4 s to write,
 * and that was the whole of the wait before the first word -- the engine itself
 * had audio 0.2 s after being asked. So the number that decides the latency
 * here is not the engine's and the naturalness measurement cannot settle it.
 *
 * 12 is a compromise against a slow writer, not a better answer than 30. A
 * faster model should raise it back.
 */
const DEFAULT_MIN_CHARS = 12

/**
 * And the point past which a piece is cut with no full stop to cut at.
 *
 * Finer than the core's 60 for the same reason the minimum is finer than its
 * 30: a reply can run 45 characters before its first full stop, and on a slow
 * writer that is the whole of the wait. The core falls back to the last comma,
 * so what this produces is a clause rather than a sentence -- shorter than the
 * unit the naturalness measurement preferred, and the price of speaking while
 * the reply is still being written.
 */
const DEFAULT_MAX_CHARS = 40

/**
 * The pieces after the first, which can wait for more text while the one before
 * them plays: 30 is the length that sounded better than 12 (see the core's
 * SplitOptions::min_chars), and 80 keeps a run-on sentence from becoming one
 * enormous utterance. The core cuts early anyway when the listener is about to
 * run out of audio, so a slow writer is not made to wait for these.
 */
const LATER_MIN_CHARS = 30
const LATER_MAX_CHARS = 80

/**
 * And the first piece, the whole of the listener's wait, may end at a full stop
 * from 6 characters on rather than DEFAULT_MIN_CHARS: "我知道苏州河。" is spoken
 * at once instead of waiting for the next clause to be written. Six keeps
 * "你好。" and "你觉得呢？", which were measured to sound clipped alone, joined to
 * what follows. See the core's SplitOptions::stop_chars.
 */
const FIRST_STOP_CHARS = 6

/**
 * The talker reads each piece's text one token per frame, not all of it before
 * the first frame: upstream's own default for a cloned voice. Listened to on
 * three passages against the other layout, it had no sentence louder than its
 * neighbours. Its endings stopped mid vowel instead, three seeds of three on one
 * passage, and EOS_HOLD_FRAMES is the fix for that: two more frames (160 ms)
 * after the talker first chooses to stop let every ending decay to silence, and
 * four sounded no different. The engine ends a piece at three times the hold
 * past that first stop regardless, since once, held on, it coughed for five
 * seconds, and drops what it made past the hold, heard once as a faint noise.
 * Both need qwentts-streaming-text.patch in the addon's speech SDK; without it
 * the engine ignores them.
 */
const STREAMING_TEXT = true
const EOS_HOLD_FRAMES = 2

/**
 * A reply read as ONE generation that takes the text as the model writes it,
 * instead of being cut into pieces that each start from the top: no seams, and
 * the pace moves across the reply the way a whole take's does. The writer has
 * to stay ahead of the talker, which reads 12.5 tokens a second; the on-device
 * Qwen3.5-2B wrote 21 while speech was being generated, and a simulated writer
 * at 21 and 15 ran nothing dry behind a half-second buffer. The 4B, at 7 while
 * speech runs, would not keep up -- false puts the pieces back, and the split
 * settings below are still applied so that works at once.
 */
const WHOLE_REPLY = true

const FILE = 'reader.json'
const EMPTY: Stored = {
  enabled: true,
  reference: '',
  bundled: '',
  minChars: 0,
  temperature: DEFAULT_TEMPERATURE,
  style: ''
}

function read(): Stored {
  const s = readJson<Stored>(dataFile(FILE), EMPTY)
  return {
    // On unless turned off.
    enabled: s.enabled !== false,
    reference: typeof s.reference === 'string' ? s.reference : '',
    bundled: typeof s.bundled === 'string' ? s.bundled : '',
    minChars: typeof s.minChars === 'number' ? s.minChars : 0,
    temperature:
      typeof s.temperature === 'number' && s.temperature >= 0 ? s.temperature : DEFAULT_TEMPERATURE,
    style: typeof s.style === 'string' && s.style in STYLES ? s.style : ''
  }
}

/**
 * Where speaker/ is. Beside the addon in a packaged app; in development
 * app.getAppPath() is examples/desktop/, so the repository root is two levels up.
 */
function speakerRoot(): string {
  if (app.isPackaged) {
    return join(process.resourcesPath, 'speaker')
  }
  return join(app.getAppPath(), '..', '..', 'speaker')
}

/**
 * The voices that come with the app: each directory under speaker/ holding a
 * .spk, by name.
 *
 * Offered in a list so a new user has a voice to choose without first finding
 * or recording one. Which .spk of a voice is used is decided at load, by the
 * talker's size.
 */
function bundledVoices(): string[] {
  const root = speakerRoot()
  if (!existsSync(root)) {
    return []
  }
  return readdirSync(root, { withFileTypes: true })
    .filter((d) => d.isDirectory() && readdirSync(join(root, d.name)).some((n) => n.endsWith('.spk')))
    .map((d) => d.name)
    .sort()
}

/** The bundled voice in use while no file is picked: the stored one if it is still there. */
function bundledName(s: Stored): string {
  return s.bundled.length > 0 && bundledVoices().includes(s.bundled) ? s.bundled : SPEAKER
}

/**
 * The .spk in `dir` for a talker `hidden` wide, or '' when there is none.
 *
 * Chosen by byte size, not by name: a .spk is `hidden` f32 values and nothing
 * else, so 4 x hidden bytes is the file that fits, and a renamed file still
 * works.
 */
function speakerFile(dir: string, hidden: number): string {
  if (hidden <= 0 || !existsSync(dir)) {
    return ''
  }
  for (const name of readdirSync(dir)) {
    const path = join(dir, name)
    if (name.endsWith('.spk') && statSync(path).size === hidden * 4) {
      return path
    }
  }
  return ''
}

/**
 * Where a recording's embedding is kept once extracted, for a talker `hidden`
 * wide.
 *
 * Extracting runs the model's speaker encoder, about 4.5 s for a 22-second
 * recording, and the vector is all that is used afterwards -- so it is paid
 * once per recording and model size rather than at every start. Keyed by the
 * file's path, size and modification time, so an edited recording is extracted
 * again.
 */
function cacheFile(wav: string, hidden: number): string {
  const st = statSync(wav)
  const key = createHash('sha1')
    .update(`${wav}\n${st.size}\n${st.mtimeMs}`)
    .digest('hex')
    .slice(0, 16)
  const dir = join(app.getPath('userData'), 'speaker-cache')
  mkdirSync(dir, { recursive: true })
  return join(dir, `${key}-${hidden}.spk`)
}

/** The reference voice the loaded engine is anchored to, as chosen; '' when none. */
let anchoredTo = ''

/** Load a .spk into the engine; '' on success, else why not. */
function anchor(spk: string, shown: string): string {
  const why = core().nativeReaderLoadReferenceEmbedding(spk)
  if (why.length > 0) {
    return why
  }
  anchoredTo = shown
  return ''
}

/**
 * Anchor the loaded engine to the chosen reference voice. Resolves to '' when
 * it is anchored, or when there is nothing loaded to anchor yet -- load() calls
 * this again -- and to the reason otherwise.
 */
function applyReference(): Promise<string> {
  anchoredTo = ''
  if (!available() || core().nativeReaderLoaded() !== 1 ||
      core().nativeReaderModelType() !== 'base') {
    return Promise.resolve('')
  }
  const hidden = core().nativeReaderTalkerHiddenSize()
  const s = read()
  const chosen = s.reference

  if (chosen.length === 0) {
    const dir = join(speakerRoot(), bundledName(s))
    const spk = speakerFile(dir, hidden)
    if (spk.length === 0) {
      return Promise.resolve(`${dir} has no .spk for a talker ${hidden} wide`)
    }
    return Promise.resolve(anchor(spk, spk))
  }
  if (!existsSync(chosen)) {
    return Promise.resolve(`no file at ${chosen}`)
  }
  // An embedding is loaded as it is; the core checks its length.
  if (!chosen.toLowerCase().endsWith('.wav')) {
    return Promise.resolve(anchor(chosen, chosen))
  }

  // A recording: its embedding from the cache when it has been extracted for
  // this model size before, and extracted now otherwise.
  const cached = cacheFile(chosen, hidden)
  if (existsSync(cached) && anchor(cached, chosen).length === 0) {
    return Promise.resolve('')
  }
  status()
  busy = true
  return new Promise<string>((resolve) => {
    core().nativeReaderSetReferenceVoice(chosen, (type, content) => {
      busy = false
      lastStatus = null
      if (type !== 'end') {
        resolve(content || 'the recording could not be used')
        return
      }
      anchoredTo = chosen
      const why = core().nativeReaderSaveReferenceEmbedding(cached)
      if (why.length > 0) {
        // The voice is anchored either way; only the next start pays again.
        console.warn(`[yoai] the reference voice was not cached: ${why}`)
      }
      resolve('')
    })
  })
}

/**
 * A take recorded in the settings screen, kept and used as the reference voice.
 *
 * Kept as the recording rather than only as its embedding, so it can be
 * extracted again for the other model size, or played back later. Under
 * userData/speaker/, one file per take, named by when it was made.
 */
export async function useRecording(wav: Uint8Array): Promise<string> {
  // A take the page could have made is seconds of 16-bit mono at 24 kHz; the
  // cap is far past that, so only something that is not a take reaches it.
  if (wav.length < 44 || wav.length > 32 * 1024 * 1024) {
    return 'not a recording this page could have made'
  }
  const dir = join(app.getPath('userData'), 'speaker')
  mkdirSync(dir, { recursive: true })
  const stamp = new Date().toISOString().replace(/[-:]/g, '').replace('T', '-').slice(0, 15)
  const path = join(dir, `recording-${stamp}.wav`)
  writeFileSync(path, wav)
  return setReference(path)
}

/**
 * Use the voice under speaker/ named `name` from now on, in place of any file
 * picked, and at once if the engine is loaded. Resolves to '' or the reason it
 * failed.
 */
export async function useBundled(name: string): Promise<string> {
  // Only a name from the list: it becomes part of a path.
  if (!bundledVoices().includes(name)) {
    return `no voice named ${name} under speaker/`
  }
  const s = read()
  s.bundled = name
  write(s)
  return setReference('')
}

/**
 * Use `path` -- a .wav or a .spk, or '' for the bundled voice -- from now on,
 * and at once if the engine is loaded. Resolves to '' or the reason it failed.
 */
export async function setReference(path: string): Promise<string> {
  const s = read()
  s.reference = path
  write(s)
  lastStatus = null
  const why = await applyReference()
  loadNote = why.length > 0 ? `the reference voice failed: ${why}` : ''
  lastStatus = null
  return why
}

function write(s: Stored): void {
  writeJson(dataFile(FILE), s)
}

export function available(): boolean {
  try {
    return core().nativeReaderAvailable() === 1
  } catch {
    return false
  }
}

/** True while loading or reading; see tts.ts for why a cached status matters. */
let busy = false
let lastStatus: ReaderStatus | null = null

export function status(): ReaderStatus {
  if (busy && lastStatus !== null) {
    return lastStatus
  }
  const s = read()
  const ok = available()
  lastStatus = {
    available: ok,
    loaded: ok ? core().nativeReaderLoaded() === 1 : false,
    modelType: ok ? core().nativeReaderModelType() : '',
    speaker: anchoredTo,
    reference: s.reference,
    bundled: bundledName(s),
    bundledVoices: bundledVoices(),
    note: loadNote,
    minChars: s.minChars,
    temperature: s.temperature,
    enabled: s.enabled,
    speakers: ok ? speakerNames() : [],
    style: s.style
  }
  return lastStatus
}

/** The named speakers of the loaded talker; [] unless it is CustomVoice. */
function speakerNames(): string[] {
  try {
    const parsed: unknown = JSON.parse(core().nativeReaderSpeakers())
    return Array.isArray(parsed) ? parsed.map(String) : []
  } catch {
    return []
  }
}

/**
 * The speaker and the style, on a CustomVoice talker; on any other, neither --
 * the engine refuses both there, and would keep one left from an earlier model.
 * Called after every load and whenever either is changed.
 */
export function applyVoice(): void {
  if (!available() || core().nativeReaderLoaded() !== 1) {
    return
  }
  if (core().nativeReaderModelType() !== 'custom_voice') {
    core().nativeReaderSetSpeaker('')
    core().nativeReaderSetVoice('')
    return
  }
  // The stored name if this talker has it, else the default, else its first:
  // a CustomVoice talker will not read without one.
  const names = speakerNames()
  const chosen = voiceSettings().speaker
  const speaker = names.includes(chosen)
    ? chosen
    : names.includes(DEFAULT_SPEAKER)
      ? DEFAULT_SPEAKER
      : (names[0] ?? '')
  core().nativeReaderSetSpeaker(speaker)
  const style = read().style
  core().nativeReaderSetVoice(style === '' ? '' : STYLES[style])
}

/** Saved, and handed to a loaded CustomVoice talker from the next reply on. */
export function setStyle(style: string): void {
  const s = read()
  s.style = style in STYLES ? (style as ReaderStyle) : ''
  write(s)
  lastStatus = null
  applyVoice()
}

export function setEnabled(on: boolean): void {
  const s = read()
  s.enabled = on
  write(s)
  lastStatus = null
}

export function setMinChars(n: number): void {
  const s = read()
  s.minChars = Number.isFinite(n) && n > 0 ? Math.trunc(n) : 0
  write(s)
  lastStatus = null
}

/**
 * Saved, and handed to the loaded engine at once: it applies from the next
 * piece. A CustomVoice talker keeps 0.9; see talkerTemperature().
 */
export function setTemperature(t: number): void {
  const s = read()
  s.temperature = Number.isFinite(t) && t >= 0 ? Math.min(t, 2) : DEFAULT_TEMPERATURE
  write(s)
  lastStatus = null
  if (available() && core().nativeReaderLoaded() === 1) {
    core().nativeReaderSetTemperature(talkerTemperature(s))
  }
}

/**
 * Read the two GGUFs. Resolves to '' on success, or to the reason.
 *
 * The paths come from the speech lane's settings, and that lane is unloaded
 * first: two engines over the same 2.2 GB is the one configuration nobody
 * wants, and it is easy to reach by accident.
 */
export function load(): Promise<string> {
  if (!available()) {
    return Promise.resolve('this build has no speech engine')
  }
  const { talkerPath, tokenizerPath } = ttsPaths()
  if (talkerPath.length === 0 || tokenizerPath.length === 0) {
    return Promise.resolve('pick the talker and tokenizer in the speech settings first')
  }
  if (!existsSync(talkerPath) || !existsSync(tokenizerPath)) {
    return Promise.resolve('one of the model files is missing')
  }
  if (core().nativeReaderLoaded() === 1) {
    return Promise.resolve('')
  }

  // One engine, not several. A second voice measured slightly SLOWER on this
  // desktop and costs another 1.4 GB -- see src/voice/tts/reader.hpp.
  const voices = 1
  try {
    if (core().nativeTtsLoaded() === 1) {
      core().nativeTtsUnload()
    }
  } catch {
    // Nothing loaded, or no speech lane in this build. Either way there is
    // nothing to make room for.
  }

  status()
  busy = true
  return new Promise<string>((resolve) => {
    core().nativeReaderLoad(talkerPath, tokenizerPath, voices, (type, content) => {
      busy = false
      lastStatus = null
      if (type !== 'end') {
        resolve(content || 'the model would not load')
        return
      }

      // Everything the voice depends on, after the load because the .spk is
      // chosen by the talker's width and the speaker from the talker's own
      // list, which only a loaded engine knows. applyVoice() sends a speaker
      // and a style to CustomVoice and clears both on Base, which takes neither.
      const s = read()
      core().nativeReaderSetSeed(voiceSettings().seed)
      core().nativeReaderSetTemperature(talkerTemperature(s))
      // Always, so 0 means this lane's default rather than the core's.
      core().nativeReaderSetSplit(
        s.minChars > 0 ? s.minChars : DEFAULT_MIN_CHARS,
        DEFAULT_MAX_CHARS,
        0
      )
      core().nativeReaderSetLaterSplit(LATER_MIN_CHARS, LATER_MAX_CHARS)
      core().nativeReaderSetFirstStopChars(FIRST_STOP_CHARS)
      core().nativeReaderSetStreamingText(STREAMING_TEXT)
      core().nativeReaderSetEosHold(EOS_HOLD_FRAMES)
      core().nativeReaderSetWholeReply(WHOLE_REPLY)
      applyVoice()
      // The setting that decides whether this lane speaks while the model is
      // still writing. Without it a comma is only a fallback at max_chars, and
      // a reply whose one full stop is at the end cannot be started at all.
      core().nativeReaderSetClauseCuts(true)
      // The reference voice, on a Base checkpoint only -- applyReference() does
      // nothing on CustomVoice. Not fatal: the lane reads without an anchor, it
      // just drifts more. Reported so a missing file does not look like nothing.
      void applyReference().then((why) => {
        resolve(why.length > 0 ? `loaded, but the reference voice failed: ${why}` : '')
      })
    })
  })
}

export function unload(): void {
  if (available() && core().nativeReaderLoaded() === 1) {
    core().nativeReaderUnload()
  }
  anchoredTo = ''
  lastStatus = null
}

/**
 * What went wrong with the last automatic load, for the settings screen; ''
 * when nothing did.
 */
let loadNote = ''

/**
 * Load the voice without being asked, whenever both model files are picked.
 *
 * **This is the only lane the desktop speaks through**: replies as they are
 * written, a message's 🔊 and the settings screen's test all come here. Having
 * to press 载入 every run was a step that existed only to be forgotten.
 *
 * **Base or CustomVoice.** Base was the only one while replies were read in
 * pieces: each piece is its own generation, only Base takes the reference
 * voice that keeps them in one register, and CustomVoice drifted -- one piece
 * in sixteen came out five semitones low. Read as one generation
 * (WHOLE_REPLY), there are no pieces to drift apart, and CustomVoice's
 * built-in speakers and its styles are offered beside Base's recorded voice.
 * VoiceDesign is a way to MAKE a reference voice, not to talk, and is
 * unloaded again and said so.
 *
 * Loaded whether or not replies are read automatically, since 🔊 needs it
 * either way. For the log -- nothing waits on this.
 */
export async function autoLoadVoice(): Promise<string> {
  const { talkerPath, tokenizerPath } = ttsPaths()
  if (!available() || talkerPath.length === 0 || tokenizerPath.length === 0) {
    return ''
  }
  if (core().nativeReaderLoaded() === 1) {
    return ''
  }
  loadNote = ''
  let reason = await load()
  if (core().nativeReaderLoaded() === 1 && core().nativeReaderModelType() === 'voice_design') {
    reason = 'needs a Base or CustomVoice talker; this one is voice_design'
    unload()
  }
  loadNote = reason
  lastStatus = null
  return reason
}

/** Hand the loaded engine the seed just saved; load() does it otherwise. */
export function applySeed(): void {
  if (available() && core().nativeReaderLoaded() === 1) {
    core().nativeReaderSetSeed(voiceSettings().seed)
  }
}

/** Abandon the open stream at its next block. It must still be finished. */
export function cancel(): void {
  if (available()) {
    core().nativeReaderCancel()
  }
}

/**
 * Open a stream. False when the engine is not loaded or one is already open.
 *
 * `onAudio` is called with 24 kHz mono chunks in reading order, and once at the
 * end with an empty one. `sentence` says which piece the samples belong to.
 */
export function begin(
  onAudio: (pcm: Float32Array, sentence: number) => void,
  onEnd: (stats: string, error: string) => void
): boolean {
  if (!available() || core().nativeReaderLoaded() !== 1) {
    return false
  }
  status()
  busy = true
  const started =
    core().nativeReaderBegin((type, payload, sentence) => {
      if (type === 'audio' && payload instanceof Float32Array && payload.length > 0) {
        onAudio(payload, typeof sentence === 'number' ? sentence : 0)
      }
    }) === 1
  if (!started) {
    busy = false
    lastStatus = null
    return false
  }

  // finish() is what tells us the stream is over, and it is called separately.
  // Held here so the caller does not have to thread it through.
  endCallback = onEnd
  return true
}

let endCallback: ((stats: string, error: string) => void) | null = null

/** Add text. Returns at once; false when no stream is open. */
export function feed(delta: string): boolean {
  if (!available() || delta.length === 0) {
    return true
  }
  return core().nativeReaderFeed(delta) === 1
}

/** No more text. The end callback fires once the last sample has gone out. */
export function finish(): void {
  if (!available()) {
    return
  }
  core().nativeReaderFinish((type, content) => {
    busy = false
    lastStatus = null
    const done = endCallback
    endCallback = null
    if (done !== null) {
      done(type === 'end' ? content : '', type === 'end' ? '' : content)
    }
  })
}
