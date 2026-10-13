/**
 * Speech recognition for a voice conversation: the SenseVoice model, and the
 * mode the microphone button works in.
 *
 * The model is a GGUF the user picks once, loaded at start like the others.
 * An utterance is recognized on a worker in the addon: 16-bit mono at 16 kHz
 * in, the core's JSON out (yoai_voice_analyze in src/yoai.h), of which a
 * conversation needs the text and the event -- whether it was speech at all.
 *
 * Recognition runs on the CPU, so unlike the speech and chat engines it holds
 * nothing the GPU exit problem in index.ts is about.
 */
import { existsSync } from 'node:fs'
import type { AsrStatus, Transcript, TurnCue, MicMode } from '../shared/api'
import { core } from './core'
import { dataFile, readJson, writeJson } from './jsonfile'

const FILE = 'asr.json'

interface Stored {
  modelPath: string
  mode: MicMode
}

const MODES = new Set<MicMode>(['talk', 'duplex'])

function read(): Stored {
  const raw = readJson<Partial<Stored>>(dataFile(FILE), {})
  return {
    modelPath: typeof raw.modelPath === 'string' ? raw.modelPath : '',
    mode: raw.mode !== undefined && MODES.has(raw.mode) ? raw.mode : 'talk'
  }
}

function write(s: Stored): void {
  writeJson(dataFile(FILE), s)
}

export function available(): boolean {
  try {
    return core().nativeVoiceAvailable() === 1
  } catch {
    return false
  }
}

// Kept here rather than asked of the core: yoai_voice_loaded() takes the lock
// a load and every recognition hold, and a status call from the settings page
// would wait out either on this process's only thread.
let loaded = false
let loading = false
let note = ''

export function status(): AsrStatus {
  const s = read()
  return {
    available: available(),
    loaded,
    loading,
    modelPath: s.modelPath,
    mode: s.mode,
    note
  }
}

/** Load the picked model. Resolves to '' when it is resident, else the reason. */
export function load(): Promise<string> {
  const s = read()
  if (!available()) {
    return Promise.resolve('this build has no speech recognition')
  }
  if (s.modelPath.length === 0) {
    return Promise.resolve('no recognition model picked')
  }
  if (!existsSync(s.modelPath)) {
    note = `the file is gone: ${s.modelPath}`
    return Promise.resolve(note)
  }
  if (loading) {
    return Promise.resolve('already loading')
  }
  loading = true
  return new Promise((resolve) => {
    core().nativeVoiceLoad(s.modelPath, (type, content) => {
      loading = false
      loaded = type === 'end'
      note = loaded ? '' : content
      resolve(note)
    })
  })
}

/** At start: load the model when one is picked. '' when there is none to load. */
export async function autoLoad(): Promise<string> {
  if (loaded || read().modelPath.length === 0) {
    return ''
  }
  return load()
}

export function unload(): void {
  if (available() && loaded && !loading) {
    core().nativeVoiceUnload()
  }
  loaded = false
}

export function setModelPath(path: string): Promise<string> {
  write({ ...read(), modelPath: path })
  unload()
  return load()
}

export function setMode(mode: MicMode): void {
  if (MODES.has(mode)) {
    write({ ...read(), mode })
  }
}

const CUES: TurnCue[] = ['answer', 'unfinished', 'correct', 'stop']

/** yoai_turn_cue, by name. */
export function cueOf(text: string): TurnCue {
  try {
    return CUES[core().nativeTurnCue(text)] ?? 'answer'
  } catch {
    return 'answer'
  }
}

/**
 * Recognize one utterance. Never rejects: a failure comes back as `error`,
 * with empty text, which the caller treats the same as having heard nothing.
 */
export function transcribe(pcm: Int16Array): Promise<Transcript> {
  const nothing = (error: string): Transcript => ({
    text: '',
    cue: 'answer',
    event: '',
    seconds: 0,
    error
  })
  if (!loaded) {
    return Promise.resolve(nothing('the recognition model is not loaded'))
  }
  if (pcm.length === 0) {
    return Promise.resolve(nothing(''))
  }
  return new Promise((resolve) => {
    core().nativeVoiceAnalyze(pcm, (type, content) => {
      if (type !== 'end') {
        resolve(nothing(content))
        return
      }
      try {
        const r = JSON.parse(content) as {
          audioSeconds?: number
          transcript?: { text?: string; event?: string }
        }
        const text = (r.transcript?.text ?? '').trim()
        resolve({
          text,
          cue: cueOf(text),
          event: r.transcript?.event ?? '',
          seconds: r.audioSeconds ?? pcm.length / 16000,
          error: ''
        })
      } catch {
        resolve(nothing('the recognizer answered with something that is not JSON'))
      }
    })
  })
}
