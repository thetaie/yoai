/**
 * Omni, from the page's side: ask, play, report.
 *
 * Thin on purpose. The engine does the whole exchange in one call, so there is
 * no turn to manage and no stream of text to assemble -- but the reply does NOT
 * arrive at the end: the thinker finishes early and hands its text over while
 * the Talker is still speaking, which is the one thing this module has to get
 * right. Measured here, 3.8 s against 70.3 s.
 *
 * It shares the player with speech output, because the speaker is one. Asking
 * omni stops whatever was being said.
 */
import type { OmniEvent } from '@shared/api'
import { player } from './audio'

/** Resolves with the reply text, or rejects with a reason. */
export interface OmniResult {
  text: string
  /** The timings as reported: speechRealtime, firstAudioSeconds, audioBlocks. */
  stats: Record<string, unknown>
}

/** 24 kHz mono, as everything on this lane's output side is. */
const SAMPLE_RATE = 24000

let unsubscribe: (() => void) | null = null
let pending: {
  resolve: (r: OmniResult) => void
  reject: (e: Error) => void
  onText?: (text: string) => void
} | null = null

/** When the ask went out, so the log can say how long the first sound took. */
let askedAt = 0
let firstChunkAt = 0
let prevChunkAt = 0
let blocks = 0
let audioSeconds = 0
/** The reply, from the 'text' event; kept in case 'end' brings no stats. */
let replyText = ''

/** Seconds since the ask, to two places. */
function at(): string {
  return ((Date.now() - askedAt) / 1000).toFixed(2)
}

function onEvent(ev: OmniEvent): void {
  if (ev.type === 'audio') {
    if (ev.pcm !== undefined) {
      const now = Date.now()
      const seconds = ev.pcm.length / SAMPLE_RATE
      blocks++
      audioSeconds += seconds
      if (firstChunkAt === 0) {
        firstChunkAt = now
        console.log(`[omni] first audio at the speaker: ${now - askedAt} ms`)
      }
      // Per block, because the summary at the end cannot tell an even trickle
      // from a stall followed by a burst, and those sound completely different.
      // The pair that decides is `in hand` against the clock: once the audio in
      // hand stops growing faster than the clock, playback has to wait.
      //
      // The `+` is the gap since the previous block. A block holding 1.2 s of
      // speech that arrives 4.3 s after the last one is 3.1 s of silence for
      // anyone playing as it arrives -- which is this lane's ratio, restated.
      console.log(
        `[omni] block ${blocks} at ${at()} s ` +
          `(+${((now - prevChunkAt) / 1000).toFixed(2)} s): ` +
          `${seconds.toFixed(2)} s of audio, ${audioSeconds.toFixed(2)} s in hand`
      )
      prevChunkAt = now
      player.push(ev.pcm)
    }
    return
  }

  if (ev.type === 'text') {
    // The words, as soon as they exist. See OmniEvent: this is most of a minute
    // before 'end' on a spoken reply, and it is the same text.
    replyText = ev.text ?? ''
    console.log(`[omni] text at ${at()} s, ${replyText.length} characters`)
    pending?.onText?.(replyText)
    return
  }

  player.flush()
  const p = pending
  pending = null

  if (ev.type === 'error') {
    console.error('[omni]', ev.error ?? 'failed')
    p?.reject(new Error(ev.error ?? 'omni failed'))
    return
  }

  let stats: Record<string, unknown> = {}
  try {
    stats = JSON.parse(ev.stats ?? '{}') as Record<string, unknown>
  } catch {
    // A summary that will not parse costs nothing to drop; the audio and the
    // text have already arrived.
  }
  // Worth having in the console while this lane is being judged: these are the
  // numbers design/voice.md's argument turns on, and they are per exchange.
  console.log(
    `[omni] done at ${at()} s: ${blocks} block(s), ${audioSeconds.toFixed(2)} s of speech`,
    stats
  )
  p?.resolve({ text: String(stats.text ?? replyText), stats })
}

function ensureSubscribed(): void {
  if (unsubscribe === null) {
    unsubscribe = window.yoai.omni.onEvent(onEvent)
  }
}

// Let go on a development reload of this module; see the same in reader.ts.
import.meta.hot?.dispose(() => {
  unsubscribe?.()
  unsubscribe = null
})

/**
 * Ask, and resolve when the whole exchange is done.
 *
 * `onText` is the half that matters to a reader: it fires when the thinker
 * stops, which is where the answer actually exists. The promise resolves much
 * later, when the Talker has finished saying it -- so a caller that only awaits
 * this shows the reply after the speech reading it has already played.
 */
export async function ask(prompt: string, onText?: (text: string) => void): Promise<OmniResult> {
  ensureSubscribed()
  player.stop()
  askedAt = Date.now()
  prevChunkAt = askedAt
  firstChunkAt = 0
  blocks = 0
  audioSeconds = 0
  replyText = ''

  const started = await window.yoai.omni.ask(prompt)
  if (!started) {
    throw new Error('omni is not loaded, or is already answering')
  }
  return new Promise<OmniResult>((resolve, reject) => {
    pending = { resolve, reject, onText }
  })
}
