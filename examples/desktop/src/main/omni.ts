/**
 * Omni: one model that hears, thinks and speaks, with no text in the middle.
 *
 * The other two lanes are halves of a chain -- llama.cpp writes the words,
 * qwentts says them -- and this one is not. That is the only thing it is
 * carried for, and docs/voice.md weighs what it costs: 3.8 GB, a second
 * inference framework, and a Talker that on this desktop measured RTF 3.1.
 *
 * A DIRECTORY, not a file. An MNN model is a config.json plus a dozen .mnn
 * graphs -- thinker, audio encoder, talker, predit, dit, bigvgan, speaker
 * dictionary -- so the picker asks for the folder that holds them.
 *
 * Audio in is 16 kHz 16-bit mono; audio out is 24 kHz float. The two rates are
 * genuinely different and nothing carries them, so the conversion lives with
 * whoever produces or plays the samples, not here.
 *
 * Until there is a microphone, the question goes in the prompt and the audio is
 * a short silence. The whole chain still runs; only recognition is untested.
 */
import { existsSync } from 'node:fs'
import { join } from 'node:path'
import type { OmniStatus } from '../shared/api'
import { core } from './core'
import { dataFile, readJson, writeJson } from './jsonfile'

interface Stored {
  modelDir: string
  /**
   * Diffusion solver steps. 0 leaves the model's own config alone.
   *
   * yoai.h calls this the one lever on the cost that does not need a
   * different model: MNN runs `steps - 1` DiT forwards per chunk, and the
   * shipped config says 5. What fewer of them coarsens is naturalness, which is
   * the whole reason this engine is here -- so the number to find is not the
   * fastest, it is the fewest that still sounds better than the alternative.
   */
  ditSteps: number
  /** Route the composer to omni instead of the chat + speech chain. */
  enabled: boolean
}

const FILE = 'omni.json'
const EMPTY: Stored = { modelDir: '', ditSteps: 0, enabled: false }

function read(): Stored {
  const s = readJson<Stored>(dataFile(FILE), EMPTY)
  return {
    modelDir: typeof s.modelDir === 'string' ? s.modelDir : '',
    ditSteps: typeof s.ditSteps === 'number' ? s.ditSteps : 0,
    enabled: s.enabled === true
  }
}

function write(s: Stored): void {
  writeJson(dataFile(FILE), s)
}

export function available(): boolean {
  try {
    return core().nativeOmniAvailable() === 1
  } catch {
    return false
  }
}

/** True while the engine is loading or answering; see tts.ts for why it matters. */
let busy = false
let lastStatus: OmniStatus | null = null

export function status(): OmniStatus {
  if (busy && lastStatus !== null) {
    return lastStatus
  }
  const s = read()
  const ok = available()
  lastStatus = {
    available: ok,
    loaded: ok ? core().nativeOmniLoaded() === 1 : false,
    modelDir: s.modelDir,
    ditSteps: s.ditSteps,
    enabled: s.enabled
  }
  return lastStatus
}

export function setModelDir(dir: string): void {
  const s = read()
  s.modelDir = dir
  write(s)
  if (available() && core().nativeOmniLoaded() === 1) {
    core().nativeOmniUnload()
  }
  lastStatus = null
}

export function setDitSteps(steps: number): void {
  const s = read()
  s.ditSteps = Number.isFinite(steps) ? Math.trunc(steps) : 0
  write(s)
  lastStatus = null
}

export function setEnabled(on: boolean): void {
  const s = read()
  s.enabled = on
  write(s)
  lastStatus = null
}

/**
 * Read the model. Resolves to '' on success, or to the reason.
 *
 * Tens of seconds and gigabytes. A directory is checked for the one file that
 * says it IS a model, so the message can name what was wrong rather than
 * leaving MNN to fail somewhere inside its own loader.
 */
export function load(): Promise<string> {
  if (!available()) {
    return Promise.resolve('this build has no omni lane -- rebuild with `python build.py clean mnn`')
  }
  const s = read()
  if (s.modelDir.length === 0) {
    return Promise.resolve('pick the model directory first')
  }
  if (!existsSync(join(s.modelDir, 'config.json'))) {
    return Promise.resolve(`no config.json under ${s.modelDir} -- is that the model directory?`)
  }
  if (core().nativeOmniLoaded() === 1) {
    return Promise.resolve('')
  }

  status()
  busy = true
  return new Promise<string>((resolve) => {
    core().nativeOmniLoad(s.modelDir, (type, content) => {
      busy = false
      lastStatus = null
      if (type === 'end' && s.ditSteps > 0) {
        // After the load, because the engine has to exist to be told.
        core().nativeOmniSetDitSteps(s.ditSteps)
      }
      resolve(type === 'end' ? '' : content || 'the omni model would not load')
    })
  })
}

export function unload(): void {
  if (available() && core().nativeOmniLoaded() === 1) {
    core().nativeOmniUnload()
  }
  lastStatus = null
}

/**
 * Ask, with `pcm` as the recording and `prompt` as the question.
 *
 * `wantAudio` false gets the text alone, which is most of the cost saved: the
 * Talker is what makes this lane expensive, and a text-only answer skips it.
 */
export function ask(
  pcm: Int16Array,
  prompt: string,
  wantAudio: boolean,
  onEvent: (type: string, payload: Float32Array | string) => boolean | void
): boolean {
  if (!available() || core().nativeOmniLoaded() !== 1) {
    return false
  }
  status()
  busy = true
  const started =
    core().nativeOmniAsk(pcm, prompt, wantAudio, (type, payload) => {
      // 'end' and 'error' are the terminal pair. 'text' arrives mid-exchange --
      // the thinker has finished but the Talker has most of the minute still to
      // run -- so it must NOT clear the flag, or a status call would go through
      // to the core and block on the mutex the exchange is holding.
      if (type === 'end' || type === 'error') {
        busy = false
        lastStatus = null
      }
      onEvent(type, payload)
    }) === 1
  if (!started) {
    busy = false
  }
  return started
}

/**
 * Stop the running exchange. The engine checks at the Talker's next block of
 * speech, so a cancel during the thinker takes effect when the Talker starts;
 * the exchange then ends with 'end' as usual. Nothing happens when none runs.
 */
export function cancel(): void {
  if (available()) {
    core().nativeOmniCancel()
  }
}
