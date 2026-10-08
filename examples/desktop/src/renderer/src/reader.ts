/**
 * Reading aloud, from the page's side: a reply while the model is still
 * writing it, and any text that already exists.
 *
 * Handed the same deltas the transcript is, so the voice starts on the first
 * sentence while the model is still on the third -- which is seconds a streamed
 * utterance cannot recover, because the text is what the engine would
 * otherwise be waiting for. Text that already exists -- a message's 🔊, the
 * settings test -- is one stream fed in one go; see tts.ts.
 *
 * One stream at a time, which the core enforces: opening a new one cancels the
 * one that is open. It shares the player with the omni lane, because the
 * speaker is one.
 */
import type { ReaderEvent } from '@shared/api'
import { player } from './audio'

let unsubscribe: (() => void) | null = null
let pending: {
  resolve: (stats: Record<string, unknown>) => void
  reject: (e: Error) => void
} | null = null

/** From a successful begin() until the stream's 'end' or 'error' arrives. */
let open = false
/** The finish() in flight, so a second call waits on the same end. */
let closing: Promise<Record<string, unknown>> | null = null
/** Set by cancel(): audio still in flight for the old stream is not played. */
let dropping = false

/** When the turn opened, so the log can say how long the first sound took. */
let openedAt = 0
let firstChunkAt = 0
let chunks = 0
let seconds = 0
let lastSentence = -1

function onEvent(ev: ReaderEvent): void {
  if (ev.type === 'audio') {
    if (!dropping && ev.pcm !== undefined && ev.pcm.length > 0) {
      const now = Date.now()
      chunks++
      seconds += ev.pcm.length / 24000
      if (firstChunkAt === 0) {
        firstChunkAt = now
        console.log(`[reader] first audio at the speaker: ${now - openedAt} ms`)
      }
      // The seams, which are the thing this lane trades for its latency. Logged
      // at the boundary only -- one line per piece rather than per chunk.
      if (ev.sentence !== undefined && ev.sentence !== lastSentence) {
        lastSentence = ev.sentence
        console.log(
          `[reader] piece ${ev.sentence} starts at ${((now - openedAt) / 1000).toFixed(2)} s ` +
            `(${seconds.toFixed(2)} s of speech so far)`
        )
      }
      player.push(ev.pcm)
    }
    return
  }

  player.flush()
  const p = pending
  pending = null
  open = false
  closing = null

  if (ev.type === 'error') {
    console.error('[reader]', ev.error ?? 'failed')
    p?.reject(new Error(ev.error ?? 'reading failed'))
    return
  }

  let stats: Record<string, unknown> = {}
  try {
    stats = JSON.parse(ev.stats ?? '{}') as Record<string, unknown>
  } catch {
    // A summary that will not parse costs nothing to drop; the audio arrived.
  }
  console.log(`[reader] done: ${chunks} chunk(s), ${seconds.toFixed(2)} s of speech`, stats)
  p?.resolve(stats)
}

function ensureSubscribed(): void {
  if (unsubscribe === null) {
    unsubscribe = window.yoai.reader.onEvent(onEvent)
  }
}

// In development a module that is edited is run again, and the copy it replaces
// keeps its subscription unless told to let go. Every edit then added a listener
// pushing the same audio into the one player, and a reply was heard several
// times over. Nothing in a built app runs this.
import.meta.hot?.dispose(() => {
  unsubscribe?.()
  unsubscribe = null
})

/**
 * Whether this lane should take the reply, asked FRESH rather than remembered.
 *
 * The same lesson the speak flag taught: the models load in the background, so
 * a value captured when the window opened says "not loaded" for the rest of the
 * session.
 */
export async function ready(): Promise<boolean> {
  const st = await window.yoai.reader.status()
  return st.available && st.loaded && st.enabled
}

/** Whether anything can be read at all -- 🔊 does not care about `enabled`. */
export async function loaded(): Promise<boolean> {
  const st = await window.yoai.reader.status()
  return st.available && st.loaded
}

/**
 * Seconds of audio to have in hand before the first word, for THIS lane.
 *
 * Higher than the player's default because what this lane stalls for is not
 * the engine. Measured on a 16-piece reply: the engine ran at 1.4x within
 * every piece, but the lane as a whole managed 0.96x -- it waited 0.6 to 2.4 s
 * between pieces for the language model to write the next one, and the core
 * reported 0.92 s of starvation over the reply. The player takes its verdict
 * from the first two blocks, which measure the engine and say 1.4x, so it
 * streams and then rides the edge: every late block is a hole cut into the
 * middle of a word, and those are heard as crackle.
 *
 * 0.6 + 0.92 is what this reply needed, so 1.2 covers most of it and the
 * shaped edges cover the rest. It costs about 0.4 s on the first word, which
 * is the trade: this is the dial between latency and dropouts, and nothing
 * else here changes that balance.
 */
const PRIME_SECONDS = 1.2

/**
 * Open a stream, cancelling the one that is open. False when the engine is not
 * loaded.
 */
export async function begin(): Promise<boolean> {
  ensureSubscribed()
  if (open) {
    await cancel()
  }
  dropping = false
  player.stop()
  player.prime(PRIME_SECONDS)
  openedAt = Date.now()
  firstChunkAt = 0
  chunks = 0
  seconds = 0
  lastSentence = -1

  const ok = await window.yoai.reader.begin()
  if (!ok) {
    console.warn('[reader] could not begin')
  }
  open = ok
  return ok
}

/**
 * Add a delta, exactly as the model wrote it. Fire and forget.
 *
 * Markdown and all: the core turns the reply into what should be spoken
 * (src/voice/text/speakable_stream.hpp), and it can only do that on the whole stream.
 * This page used to clean fragments itself and lost every line break that fell
 * at a fragment's edge -- a list's "上海" and "南接" were read as one word.
 *
 * Not awaited by the caller: this runs once per token, and an IPC round trip
 * per token would put the page's own rendering behind the main process.
 */
export function feed(delta: string): void {
  if (delta.length === 0) {
    return
  }
  void window.yoai.reader.feed(delta)
}

/**
 * No more text. Resolves with the timings once the last sample has been
 * generated -- not played: the player still has what it was given.
 */
export function finish(): Promise<Record<string, unknown>> {
  if (!open) {
    return Promise.resolve({})
  }
  if (closing !== null) {
    return closing
  }
  closing = new Promise<Record<string, unknown>>((resolve, reject) => {
    pending = { resolve, reject }
  })
  void window.yoai.reader.finish()
  return closing
}

/**
 * Stop reading: the engine abandons the stream and what it already sent is not
 * played. Resolves once the stream is closed, so a begin() can follow. Safe
 * with nothing open.
 */
export async function cancel(): Promise<void> {
  if (!open) {
    return
  }
  dropping = true
  player.stop()
  await window.yoai.reader.cancel()
  await finish().catch(() => undefined)
}
