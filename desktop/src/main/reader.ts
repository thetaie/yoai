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
 * **It is a SECOND engine over the same two GGUFs**, so the two lanes hold the
 * weights twice if both are loaded. Loading either one here unloads the other.
 * That is a deliberate, reversible arrangement: the speech lane works and this
 * one is being judged, so the day it wins the other goes rather than the two
 * being merged.
 *
 * The model paths are tts.json's -- the same talker and tokenizer -- because
 * they ARE the same files and a second pair of pickers would be two places to
 * keep right. What this lane adds is its own: a reference recording, which is
 * what holds the voice steady across pieces, and how coarsely to cut.
 */
import { existsSync } from 'node:fs'
import type { ReaderStatus } from '../shared/api'
import { core } from './core'
import { dataFile, readJson, writeJson } from './jsonfile'
import { paths as ttsPaths, voiceSettings } from './tts'

interface Stored {
  /** Route replies through this lane instead of speaking them at the end. */
  enabled: boolean

  /**
   * A recording to sound like, WITHOUT its transcript -- qwen.h's clone mode A.
   *
   * Base checkpoints only, and the reason it is here rather than optional: every
   * piece is a separate generation, and with nothing acoustic in common they
   * drift. One sentence in sixteen came out five semitones low, which is heard
   * as the voice changing mid-reply. Anchoring every piece to one embedding
   * removed it on four seeds out of four and cost nothing measurable in speed.
   */
  referenceWav: string

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
 * 16 is a compromise against a slow writer, not a better answer than 30. A
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

const FILE = 'reader.json'
const EMPTY: Stored = { enabled: false, referenceWav: '', minChars: 0 }

function read(): Stored {
  const s = readJson<Stored>(dataFile(FILE), EMPTY)
  return {
    enabled: s.enabled === true,
    referenceWav: typeof s.referenceWav === 'string' ? s.referenceWav : '',
    minChars: typeof s.minChars === 'number' ? s.minChars : 0
  }
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
    referenceWav: s.referenceWav,
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

export function setReferenceWav(path: string): void {
  const s = read()
  s.referenceWav = path
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
  // desktop and costs another 1.4 GB -- see src/voice/reader.hpp.
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

      // Everything the voice depends on, after the load because the reference
      // extraction runs the model's own speaker encoder.
      const s = read()
      const v = voiceSettings()
      core().nativeReaderSetVoice(v.voice)
      core().nativeReaderSetSpeaker(v.speaker)
      core().nativeReaderSetSeed(v.seed)
      // Always, so 0 means this lane's default rather than the core's.
      core().nativeReaderSetSplit(
        s.minChars > 0 ? s.minChars : DEFAULT_MIN_CHARS,
        DEFAULT_MAX_CHARS,
        0
      )
      // The setting that decides whether this lane speaks while the model is
      // still writing. Without it a comma is only a fallback at max_chars, and
      // a reply whose one full stop is at the end cannot be started at all.
      core().nativeReaderSetClauseCuts(true)
      if (s.referenceWav.length > 0) {
        const why = core().nativeReaderSetReferenceVoice(s.referenceWav)
        if (why.length > 0) {
          // Not fatal: the lane reads perfectly well without an anchor, it just
          // drifts more. Reported so a wrong path does not look like nothing.
          resolve(`loaded, but the reference voice failed: ${why}`)
          return
        }
      }
      resolve('')
    })
  })
}

export function unload(): void {
  if (available() && core().nativeReaderLoaded() === 1) {
    core().nativeReaderUnload()
  }
  lastStatus = null
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
