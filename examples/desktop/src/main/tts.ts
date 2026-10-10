/**
 * Speech output: which two models to use, and whether they are loaded.
 *
 * The same split models.ts is built on -- this module owns the stored choice
 * and the core owns the engine -- with one difference that comes from the
 * engine rather than from us: there is exactly ONE voice, not a list. The core
 * keeps a single Qwen3-TTS instance behind a mutex, so a second utterance would
 * queue behind the first rather than run beside it, and the UI is told no
 * instead.
 *
 * Two files, not one. Qwen3-TTS is a talker (the autoregressive half, which
 * writes the audio codes) plus a 12 Hz audio TOKENIZER (which turns them into
 * samples). They are separate GGUFs and both are required; design/voice.md
 * measures that second stage -- it calls it the codec, being the decoding half
 * of the tokenizer -- at about 45% of the bill, which is why it is not an
 * implementation detail worth hiding.
 *
 * `tokenizer` and not `codec` for the FILE: it is named qwen-tokenizer-12hz-*
 * on disk, qwentts prints `tokenizer=qwen3_tts_tokenizer_12hz` when it loads,
 * and yoai.h's own prose calls it the 12 Hz audio tokenizer even though
 * its parameter kept the older name.
 *
 * Nothing here holds audio. PCM crosses to the renderer as it arrives and the
 * page owns playback, because only the page has an AudioContext.
 */
import { existsSync } from 'node:fs'
import type { TtsMissing, TtsSlot, TtsStatus, VoiceMode } from '../shared/api'
import { core } from './core'
import { dataFile, readJson, writeJson } from './jsonfile'
import { KNOWN } from './ttsfiles'

interface Stored {
  /** The Base talker, which voiceMode 'base' reads with. */
  talkerPath: string
  /** The CustomVoice talker, which voiceMode 'custom' reads with. */
  customTalkerPath: string
  /**
   * Which of the two talkers the voice comes from. 'base' clones a reference
   * recording; 'custom' uses a speaker built into the model and can be asked
   * for a style. Both are offered: the built-in speakers sound better today,
   * and a recorded voice is the only way to have one's own.
   */
  voiceMode: VoiceMode
  tokenizerPath: string
  /** The written voice. See DEFAULT_VOICE. */
  voice: string
  /** Read every reply aloud as it is written. */
  autoSpeak: boolean
  /** Which named speaker, on a custom_voice checkpoint. '' = the engine's own. */
  speaker: string
  /** Sampling seed. -1 draws a fresh one per call; see DEFAULT_SEED. */
  seed: number
  /** The folder the models were last downloaded into, offered first next time. */
  downloadDir: string
}

/**
 * The voice, described in English, because on this checkpoint that IS the
 * voice: yoai.h says an instruct is REQUIRED by voice_design, optional on
 * custom_voice and refused by base. Without one, say() fails -- which is not
 * obvious from the outside, because loading succeeds and the failure only
 * arrives on the first utterance.
 *
 * Not "gentle, speaks slowly": the header records that wording coming out
 * audibly listless, because the words land. This one asks for pace and presence
 * rather than calm.
 */
const DEFAULT_VOICE = 'clear, natural pace, close to the microphone'

/**
 * A FIXED seed, where the ABI's own default is -1, "a fresh one per call".
 *
 * Measured by hashing the PCM: the same sentence, same speaker, same instruct,
 * synthesised twice with the seed unset, came back as two different recordings
 * (different length, different bytes); with the seed pinned, the two runs were
 * byte-identical. The speaker fixes WHO is talking and the instruct describes
 * how, but neither fixes the draw -- so leaving the seed alone is why the test
 * button and a spoken reply did not sound like the same person.
 *
 * Pinning is the point; which value is not, mostly. Every piece opens on the
 * same prompt, so a seed's first draws land alike across texts and set the
 * tone of what follows -- the one part of a delivery a seed carries from one
 * text to the next. 101 because it is the seed with rhythm: over three passages
 * at four talker temperatures from 0.9 to 1.3, it had the widest pace swing
 * between pieces in 6 of the 12 and was in the top two of four seeds in 10,
 * and by ear it was the one preferred at 1.3 (see DEFAULT_TEMPERATURE in
 * reader.ts).
 */
const DEFAULT_SEED = 101

const FILE = 'tts.json'
const EMPTY: Stored = {
  talkerPath: '',
  customTalkerPath: '',
  voiceMode: 'base',
  tokenizerPath: '',
  voice: DEFAULT_VOICE,
  autoSpeak: false,
  speaker: '',
  seed: DEFAULT_SEED,
  downloadDir: ''
}

function read(): Stored {
  // `codecPath` is what this field was called for one afternoon. Read as a
  // fallback rather than dropped, because the alternative is silently forgetting
  // a path the user already picked and showing them an empty slot.
  const s = readJson<Stored & { codecPath?: string }>(dataFile(FILE), EMPTY)
  const voice = typeof s.voice === 'string' && s.voice.trim().length > 0 ? s.voice : DEFAULT_VOICE
  const tokenizer =
    typeof s.tokenizerPath === 'string' && s.tokenizerPath.length > 0
      ? s.tokenizerPath
      : typeof s.codecPath === 'string'
        ? s.codecPath
        : ''
  return {
    talkerPath: typeof s.talkerPath === 'string' ? s.talkerPath : '',
    customTalkerPath: typeof s.customTalkerPath === 'string' ? s.customTalkerPath : '',
    voiceMode: s.voiceMode === 'custom' ? 'custom' : 'base',
    tokenizerPath: tokenizer,
    voice,
    autoSpeak: s.autoSpeak === true,
    speaker: typeof s.speaker === 'string' ? s.speaker : '',
    seed: typeof s.seed === 'number' ? s.seed : DEFAULT_SEED,
    downloadDir: typeof s.downloadDir === 'string' ? s.downloadDir : ''
  }
}

function write(s: Stored): void {
  writeJson(dataFile(FILE), s)
}

/** True when this build has the engine at all (`build.py qtts` turns it on). */
/**
 * The two model files, for a lane that needs the same pair.
 *
 * Exposed rather than copied because they ARE the same files: reader.ts drives
 * a second engine over this one's talker and tokenizer, and a second pair of
 * pickers would be two places to keep right.
 */
export function paths(): { talkerPath: string; tokenizerPath: string } {
  const s = read()
  return {
    talkerPath: s.voiceMode === 'custom' ? s.customTalkerPath : s.talkerPath,
    tokenizerPath: s.tokenizerPath
  }
}

/** Base or CustomVoice. The caller reloads the voice: its talker has changed. */
export function setVoiceMode(mode: VoiceMode): void {
  const s = read()
  s.voiceMode = mode === 'custom' ? 'custom' : 'base'
  write(s)
}

/** The talker slot the current voice mode loads. */
export function talkerSlot(): TtsSlot {
  return read().voiceMode === 'custom' ? 'customTalker' : 'talker'
}

function pathOf(s: Stored, slot: TtsSlot): string {
  return slot === 'talker' ? s.talkerPath : slot === 'customTalker' ? s.customTalkerPath : s.tokenizerPath
}

/** The slots with no file: never picked, or picked and since gone. */
export function emptySlots(): TtsSlot[] {
  const s = read()
  return (['talker', 'customTalker', 'tokenizer'] as const).filter((slot) => {
    const p = pathOf(s, slot)
    return p.length === 0 || !existsSync(p)
  })
}

/** Of those, the ones the current voice mode loads: its talker, then the tokenizer. */
export function missingSlots(): TtsSlot[] {
  const empty = emptySlots()
  return ([talkerSlot(), 'tokenizer'] as const).filter((slot) => empty.includes(slot))
}

function missing(): TtsMissing[] {
  return missingSlots().map((slot) => ({ slot, name: KNOWN[slot].name, size: KNOWN[slot].size }))
}

/** Where the models were last downloaded to; '' before the first time. */
export function downloadDir(): string {
  return read().downloadDir
}

export function setDownloadDir(dir: string): void {
  const s = read()
  s.downloadDir = dir
  write(s)
}

/** The voice, by the same three routes, for the same reason as paths(). */
export function voiceSettings(): { voice: string; speaker: string; seed: number } {
  const s = read()
  return { voice: s.voice, speaker: s.speaker, seed: s.seed }
}

export function available(): boolean {
  try {
    return core().nativeTtsAvailable() === 1
  } catch {
    // No addon. coreStatus already says so; this must not throw on top of it.
    return false
  }
}

/**
 * The last status handed out, and whether an utterance is in flight.
 *
 * yoai_tts_say holds the core's TTS mutex for the WHOLE synthesis, and
 * every status entry point -- loaded, model_type, speakers, last_error --
 * takes that same mutex. So asking "is it loaded?" while it is speaking blocks
 * the MAIN PROCESS'S JS THREAD for seconds, and the audio chunks the engine is
 * already producing sit in the threadsafe-function queue with nobody to drain
 * them. Measured: the engine emitted its first chunk at 151 ms and all 18
 * arrived here at 5359 ms, three milliseconds after the synthesis ended and
 * the mutex was released.
 *
 * The renderer does ask, and innocently: the thread refreshes this whenever the
 * message count changes, which is exactly when a reply has just finished and
 * speaking has just started.
 *
 * So while an utterance is running, the last answer is repeated instead. None
 * of these fields can change during a say -- the models cannot be swapped,
 * unloaded or repicked while the engine is busy with them.
 */
let lastStatus: TtsStatus | null = null
/** True while the core's TTS mutex is held: during a load AND during a say. */
let busy = false

export function status(): TtsStatus {
  if (busy && lastStatus !== null) {
    return lastStatus
  }
  const s = read()
  if (!available()) {
    return {
      available: false,
      loaded: false,
      modelType: '',
      voiceMode: s.voiceMode,
      talkerPath: s.talkerPath,
      customTalkerPath: s.customTalkerPath,
      tokenizerPath: s.tokenizerPath,
      voice: s.voice,
      autoSpeak: s.autoSpeak,
      speaker: s.speaker,
      speakers: [],
      seed: s.seed,
      lastError: '',
      missing: missing()
    }
  }
  lastStatus = {
    available: true,
    loaded: core().nativeTtsLoaded() === 1,
    modelType: core().nativeTtsModelType(),
    voiceMode: s.voiceMode,
    talkerPath: s.talkerPath,
    customTalkerPath: s.customTalkerPath,
    tokenizerPath: s.tokenizerPath,
    voice: s.voice,
    autoSpeak: s.autoSpeak,
    speaker: s.speaker,
    // Read from the loaded model, not from a list kept here: yoai.h says
    // "nine named speakers" was taken from a model card once and never checked,
    // and a list that came out of the file cannot be stale.
    speakers: speakerNames(),
    seed: s.seed,
    lastError: core().nativeTtsLastError(),
    missing: missing()
  }
  return lastStatus
}

/**
 * Remember one of the two files. Unloads first when something is resident: the
 * loaded engine is the OLD pair, and leaving it up while the stored paths say
 * something else is the kind of disagreement that shows up later as a voice
 * nobody asked for.
 */
export function setPath(which: TtsSlot, path: string): void {
  const s = read()
  if (which === 'talker') {
    s.talkerPath = path
  } else if (which === 'customTalker') {
    s.customTalkerPath = path
  } else {
    s.tokenizerPath = path
  }
  write(s)

  if (available() && core().nativeTtsLoaded() === 1) {
    core().nativeTtsUnload()
  }
}

/**
 * Read both models. Resolves to '' on success, or to the reason it failed.
 *
 * Seconds and gigabytes, on a worker thread inside the addon. The file checks
 * happen here rather than in the core so the message can name the missing path,
 * which "could not load" would not.
 */
export function load(): Promise<string> {
  if (!available()) {
    return Promise.resolve('this build has no speech output -- rebuild with `python build.py qtts`')
  }
  const s = read()
  if (s.talkerPath.length === 0 || s.tokenizerPath.length === 0) {
    return Promise.resolve('pick both a talker and a tokenizer model first')
  }
  for (const p of [s.talkerPath, s.tokenizerPath]) {
    if (!existsSync(p)) {
      return Promise.resolve(`no file at ${p}`)
    }
  }
  if (core().nativeTtsLoaded() === 1) {
    return Promise.resolve('')
  }

  // Primed before the lock is taken, so the copy served while loading is the
  // current one. yoai_tts_load holds the same mutex a say does, and a
  // status call landing on it would block the main process for the whole read.
  status()
  busy = true

  return new Promise<string>((resolve) => {
    // The addon calls back exactly once, on the JS thread.
    core().nativeTtsLoad(s.talkerPath, s.tokenizerPath, (type, content) => {
      busy = false
      lastStatus = null // the model type and speakers are only known now
      resolve(type === 'end' ? '' : content || 'could not load the voice')
    })
  })
}

/**
 * Load the models at startup when both are configured, without being asked.
 *
 * Picking two files and then having to press 载入 every run is a step that can
 * only be got wrong; the paths ARE the configuration. Fire and forget, on the
 * addon's worker thread, so the window opens while it reads.
 *
 * Returns what happened, for the log only -- nothing waits on this.
 */
export async function autoLoad(): Promise<string> {
  const s = read()
  if (!available() || s.talkerPath.length === 0 || s.tokenizerPath.length === 0) {
    return ''
  }
  if (core().nativeTtsLoaded() === 1) {
    return ''
  }
  const reason = await load()
  return reason
}

export function unload(): void {
  if (available() && core().nativeTtsLoaded() === 1) {
    core().nativeTtsUnload()
  }
}

/**
 * Speak `text`, forwarding every chunk to `onEvent`. False when an utterance is
 * already running -- the core would serialise them, and hearing the previous
 * reply begin after asking for this one is worse than being told no.
 *
 * Streaming is ON. design/voice.md measured it costing nothing against the
 * buffered path, and it is what lets playback start before the whole utterance
 * exists: below RTF 1 the pre-buffer is zero, and above it the wait is
 * duration x (RTF - 1) instead of the whole duration.
 */
export function say(
  text: string,
  onEvent: (type: string, payload: Float32Array | string) => void
): boolean {
  if (!available() || core().nativeTtsLoaded() !== 1) {
    return false
  }

  // The voice, applied per utterance rather than once at load, because it is
  // cheap (it takes effect on the next say and the weights stay resident) and
  // because applying it at load would silently keep using the old description
  // after the user edited it.
  //
  // Which checkpoint is loaded decides whether this is allowed at all: the
  // header says voice_design REQUIRES an instruct, custom_voice takes one
  // optionally, and base REFUSES one. Sending it to base would turn a working
  // configuration into a failing one, so the type is asked first.
  const type = core().nativeTtsModelType()
  const s = read()
  if (type === 'voice_design' || type === 'custom_voice') {
    core().nativeTtsSetVoice(s.voice)
  }

  // The speaker, and on custom_voice this is what makes reading a reply one
  // sentence at a time viable at all. Each sentence is an independent
  // generation, and a written description pins identity only statistically --
  // consecutive utterances drift enough to hear. A speaker is a token the
  // engine inserts, so identity is fixed structurally and the seams stop being
  // audible. Illegal anywhere else, though the core guards that too.
  if (type === 'custom_voice') {
    core().nativeTtsSetSpeaker(s.speaker)
  }

  // Before every utterance, for the same reason the instruct is: the engine
  // keeps whatever it was last told, and these are settings the user can change
  // between one reply and the next.
  core().nativeTtsSetSeed(s.seed)

  core().nativeTtsSetStreaming(1)

  // Refreshed before the call, so the cached copy served during the utterance
  // is the current one rather than whatever was last asked for.
  status()

  const started =
    core().nativeTtsSay(text, (type, payload) => {
      if (type !== 'audio') {
        busy = false
      }
      onEvent(type, payload)
    }) === 1
  busy = started
  return started
}

/** The written voice, and the reason a voice_design checkpoint can speak. */
export function voice(): string {
  return read().voice
}

/** The speaker names the loaded checkpoint carries; [] for the other kinds. */
function speakerNames(): string[] {
  try {
    const parsed: unknown = JSON.parse(core().nativeTtsSpeakers())
    return Array.isArray(parsed) ? parsed.map(String) : []
  } catch {
    return []
  }
}

export function setSeed(seed: number): void {
  const s = read()
  s.seed = Number.isFinite(seed) ? Math.trunc(seed) : DEFAULT_SEED
  write(s)
}

export function setSpeaker(name: string): void {
  const s = read()
  s.speaker = name
  write(s)
}

export function setAutoSpeak(on: boolean): void {
  const s = read()
  s.autoSpeak = on
  write(s)
}

export function setVoice(text: string): void {
  const s = read()
  s.voice = text.trim().length > 0 ? text.trim() : DEFAULT_VOICE
  write(s)
}

export function cancel(): void {
  if (available()) {
    core().nativeTtsCancel()
  }
}
