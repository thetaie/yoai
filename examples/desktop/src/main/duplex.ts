/**
 * A full-duplex voice session, the main process's half.
 *
 * The page streams blocks of two signals over the same span: the microphone,
 * 16-bit at 16 kHz, and what the speaker played, float at 24 kHz -- silence
 * included, since the canceller pairs the two by position (src/yoai.h). Here
 * the echo is taken out, speech is found in what is left, and each utterance
 * is recognized. The page is told 'speech' when a voice starts, 'silence' when
 * it stops, and 'utterance' with what it said; what to do about it is the
 * page's, which knows whether a reply is playing.
 *
 * All on this process's thread except recognition: the canceller asks for one
 * thread and costs about 1% of real time, and finding speech is arithmetic.
 *
 * Finding speech is an energy gate on the echo-cancelled signal, against a
 * floor that follows the room. The canceller leaves echo it cannot model
 * (slides: "AEC: take the reply out of the microphone"), so while the speaker
 * plays the floor is let rise faster, onto that residue. What still gets
 * through is caught later, by its words: the page drops an utterance that
 * repeats the reply.
 */
import type { WebContents } from 'electron'
import { CHANNELS, DuplexEvent } from '../shared/api'
import * as asr from './asr'
import { core } from './core'

const RATE = 16000
/** Speech is judged a frame at a time: 16ms, the canceller's own block. */
const FRAME = 256

/** How far above the floor a frame must be to count as voice. */
const VOICE_OVER_FLOOR_DB = 12
/**
 * The same while the speaker plays. The canceller puts out more than the room
 * then even with no echo to take out: fed a microphone of -71 dBFS noise and
 * a reference of speech, its output rose to -57..-64 dBFS in bursts, which
 * 12 dB over the floor counted as a voice and recognition wrote down as "我",
 * "这", "The". Real echo adds its residue on top of that.
 */
const VOICE_OVER_FLOOR_PLAYING_DB = 18
/**
 * And never quieter than this while the speaker plays. Those bursts reached
 * -45 dBFS in a 16ms frame and mostly stayed under -55; speech said to the
 * microphone measured -23 to -25 dBFS. The floor alone let them through: a
 * long reply was answered with "Oh. Yeah. The." added to it.
 */
const VOICE_MIN_PLAYING_DBFS = -40
/** And above this however quiet the room: a whisper at the far side is not a turn. */
const VOICE_MIN_DBFS = -50
/** Where the floor starts, before the room has been heard. */
const FLOOR_START_DBFS = -60
/** The floor falls to any quieter frame at once, and rises at most this much a frame: 1.25 dB a second, slower than speech can lift it. */
const FLOOR_RISE_DB = 0.02
/** While the speaker plays: 6 dB a second, onto the echo the canceller leaves. */
const FLOOR_RISE_ECHO_DB = 0.1
/** The reference counts as playing above this. */
const REF_ACTIVE_DBFS = -45

/** A voice starts when this many of the last ONSET_WINDOW frames are voiced: 96 of 160ms. */
const ONSET_FRAMES = 6
const ONSET_WINDOW = 10
/** While the speaker plays, nearly all of them: the canceller's bursts are shorter. */
const ONSET_FRAMES_PLAYING = 9
/**
 * An utterance with less voice than this in it is not recognized at all: 256ms.
 * What gets past the gate in bursts adds up to less, and no word is shorter.
 */
const MIN_VOICED_FRAMES = 16
/** And ends after this long with no voiced frame: 700ms, the pause an utterance is cut at. */
const END_FRAMES = 44
/** Kept from before the start, so the first syllable is not clipped: 300ms. */
const PREROLL_FRAMES = 19
/** Cut here even mid-speech, so a long turn is not answered only once it is over: 20s. */
const MAX_FRAMES = Math.round((20 * RATE) / FRAME)

interface Session {
  sender: WebContents
  floor: number
  /** Cleaned samples short of a frame, carried to the next block. */
  carry: Int16Array
  /** The last frames before a voice starts: its pre-roll. */
  recent: Int16Array[]
  /** Whether each of the last ONSET_WINDOW frames was voiced. */
  voiced: boolean[]
  speaking: boolean
  utterance: Int16Array[]
  /** Voiced frames in the utterance so far. */
  voicedFrames: number
  /** Unvoiced frames in a row, while speaking. */
  quiet: number
  /** Recognitions in the order their utterances ended. */
  queue: Promise<void>
}

let session: Session | null = null

function emit(s: Session, ev: DuplexEvent): void {
  if (!s.sender.isDestroyed()) {
    s.sender.send(CHANNELS.duplexEvent, ev)
  }
}

function dbfs16(f: Int16Array): number {
  let sum = 0
  for (let i = 0; i < f.length; i++) {
    sum += f[i] * f[i]
  }
  const rms = Math.sqrt(sum / Math.max(1, f.length)) / 32768
  return rms > 0 ? 20 * Math.log10(rms) : -120
}

function dbfsFloat(f: Float32Array): number {
  let sum = 0
  for (let i = 0; i < f.length; i++) {
    sum += f[i] * f[i]
  }
  const rms = Math.sqrt(sum / Math.max(1, f.length))
  return rms > 0 ? 20 * Math.log10(rms) : -120
}

/** Open a session for `sender`. '' when it is open, else the reason. */
export function start(sender: WebContents): string {
  if (!asr.status().loaded) {
    return 'the recognition model is not loaded'
  }
  let reason: string
  try {
    reason = core().nativeAecReset(false)
  } catch (e) {
    reason = e instanceof Error ? e.message : String(e)
  }
  if (reason.length > 0) {
    return reason
  }
  session = {
    sender,
    floor: FLOOR_START_DBFS,
    carry: new Int16Array(0),
    recent: [],
    voiced: [],
    speaking: false,
    utterance: [],
    voicedFrames: 0,
    quiet: 0,
    queue: Promise.resolve()
  }
  console.log('[duplex] open')
  return ''
}

/** Close the session; an utterance still being recognized is not reported. */
export function stop(): void {
  if (session !== null) {
    console.log('[duplex] closed')
  }
  session = null
}

/** One block from the page: `mic` at 16 kHz, `ref` at 24 kHz, over the same span. */
export function audio(mic: Int16Array, ref: Float32Array): void {
  const s = session
  if (s === null) {
    return
  }
  if (s.sender.isDestroyed()) {
    stop()
    return
  }
  let clean: Int16Array
  try {
    clean = core().nativeAecProcess(mic, ref)
  } catch (e) {
    emit(s, { type: 'error', error: e instanceof Error ? e.message : String(e) })
    stop()
    return
  }
  const playing = dbfsFloat(ref) > REF_ACTIVE_DBFS

  let samples = clean
  if (s.carry.length > 0) {
    samples = new Int16Array(s.carry.length + clean.length)
    samples.set(s.carry)
    samples.set(clean, s.carry.length)
  }
  let at = 0
  for (; at + FRAME <= samples.length; at += FRAME) {
    frame(s, samples.slice(at, at + FRAME), playing)
    if (session !== s) {
      return
    }
  }
  s.carry = samples.slice(at)
}

function frame(s: Session, f: Int16Array, playing: boolean): void {
  const e = dbfs16(f)
  if (e < s.floor) {
    s.floor = e
  } else if (!s.speaking) {
    s.floor = Math.min(e, s.floor + (playing ? FLOOR_RISE_ECHO_DB : FLOOR_RISE_DB))
  }
  const voiced = playing
    ? e > Math.max(s.floor + VOICE_OVER_FLOOR_PLAYING_DB, VOICE_MIN_PLAYING_DBFS)
    : e > Math.max(s.floor + VOICE_OVER_FLOOR_DB, VOICE_MIN_DBFS)

  s.voiced.push(voiced)
  if (s.voiced.length > ONSET_WINDOW) {
    s.voiced.shift()
  }

  if (!s.speaking) {
    s.recent.push(f)
    if (s.recent.length > PREROLL_FRAMES) {
      s.recent.shift()
    }
    const onset = s.voiced.filter(Boolean).length
    if (onset >= (playing ? ONSET_FRAMES_PLAYING : ONSET_FRAMES)) {
      s.speaking = true
      s.utterance = s.recent
      s.recent = []
      s.voicedFrames = onset
      s.quiet = 0
      emit(s, { type: 'speech' })
    }
    return
  }

  s.utterance.push(f)
  if (voiced) {
    s.voicedFrames++
  }
  s.quiet = voiced ? 0 : s.quiet + 1
  if (s.quiet >= END_FRAMES || s.utterance.length >= MAX_FRAMES) {
    finishUtterance(s)
  }
}

function finishUtterance(s: Session): void {
  const frames = s.utterance
  const voicedFrames = s.voicedFrames
  s.speaking = false
  s.utterance = []
  s.voiced = []
  s.voicedFrames = 0
  s.quiet = 0
  emit(s, { type: 'silence' })

  // Too little voice to be a word: said to be nothing, without asking, so a
  // reply paused for it goes on at once. In order behind any recognition
  // still running, like every other answer.
  if (voicedFrames < MIN_VOICED_FRAMES) {
    s.queue = s.queue.then(() => {
      if (session === s) {
        emit(s, {
          type: 'utterance',
          transcript: { text: '', cue: 'answer', event: '', seconds: 0, error: '' }
        })
      }
    })
    return
  }

  const pcm = new Int16Array(frames.length * FRAME)
  frames.forEach((f, i) => pcm.set(f, i * FRAME))
  s.queue = s.queue.then(async () => {
    const t0 = Date.now()
    const transcript = await asr.transcribe(pcm)
    console.log(
      `[duplex] ${(pcm.length / RATE).toFixed(2)} s recognized in ${Date.now() - t0} ms: ` +
        `${transcript.cue} ${JSON.stringify(transcript.text)}${transcript.error ? ` (${transcript.error})` : ''}`
    )
    if (session === s) {
      emit(s, { type: 'utterance', transcript })
    }
  })
}
