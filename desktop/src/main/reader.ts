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
import { existsSync, mkdirSync, readdirSync, statSync } from 'node:fs'
import { join } from 'node:path'
import type { ReaderStatus } from '../shared/api'
import { core } from './core'
import { dataFile, readJson, writeJson } from './jsonfile'
import { paths as ttsPaths, voiceSettings } from './tts'

/**
 * The voice a Base checkpoint is anchored to when the user has not picked one:
 * a directory under speaker/ at the repository root, holding one .spk per model
 * size.
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
   * already extracted (.spk). '' for SPEAKER, the bundled one.
   */
  reference: string

  /**
   * How much text must have settled before a piece is sent.
   *
   * 0 means DEFAULT_MIN_CHARS below, not the core's own default -- see there.
   */
  minChars: number
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

const FILE = 'reader.json'
const EMPTY: Stored = { enabled: true, reference: '', minChars: 0 }

function read(): Stored {
  const s = readJson<Stored>(dataFile(FILE), EMPTY)
  return {
    // On unless turned off.
    enabled: s.enabled !== false,
    reference: typeof s.reference === 'string' ? s.reference : '',
    minChars: typeof s.minChars === 'number' ? s.minChars : 0
  }
}

/**
 * Where SPEAKER is. Beside the addon in a packaged app; in development
 * app.getAppPath() is desktop/, so the repository root is one level up.
 */
function speakerDir(): string {
  if (app.isPackaged) {
    return join(process.resourcesPath, 'speaker', SPEAKER)
  }
  return join(app.getAppPath(), '..', 'speaker', SPEAKER)
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
  const chosen = read().reference

  if (chosen.length === 0) {
    const dir = speakerDir()
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
    note: loadNote,
    minChars: s.minChars,
    enabled: s.enabled
  }
  return lastStatus
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
      // chosen by the talker's width, which only a loaded engine knows. No
      // instruct and no named speaker: a Base checkpoint takes neither.
      const s = read()
      core().nativeReaderSetSeed(voiceSettings().seed)
      // Always, so 0 means this lane's default rather than the core's.
      core().nativeReaderSetSplit(
        s.minChars > 0 ? s.minChars : DEFAULT_MIN_CHARS,
        DEFAULT_MAX_CHARS,
        0
      )
      core().nativeReaderSetLaterSplit(LATER_MIN_CHARS, LATER_MAX_CHARS)
      // The setting that decides whether this lane speaks while the model is
      // still writing. Without it a comma is only a fallback at max_chars, and
      // a reply whose one full stop is at the end cannot be started at all.
      core().nativeReaderSetClauseCuts(true)
      // The reference voice, on a Base checkpoint only; autoLoadVoice()
      // refuses the other two. Not fatal: the lane reads without an anchor, it
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
 * **Base checkpoints only.** Every piece is its own generation, and only a Base
 * checkpoint takes the reference voice that keeps them in one register; the
 * other two drift -- one piece in sixteen came out five semitones low on
 * custom_voice. VoiceDesign is a way to MAKE a reference voice, not to talk.
 * Anything else is unloaded again and said so, rather than read in a voice
 * that wanders.
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
  if (core().nativeReaderLoaded() === 1 && core().nativeReaderModelType() !== 'base') {
    reason = `needs a base model; this talker is ${core().nativeReaderModelType()}`
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
