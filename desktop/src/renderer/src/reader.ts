/**
 * Reading a reply aloud while the model is still writing it, from the page's
 * side.
 *
 * The speech lane in tts.ts waits for the whole reply and then speaks it. This
 * one is handed the same deltas the transcript is, so the voice starts on the
 * first sentence while the model is still on the third -- which is seconds a
 * streamed utterance cannot recover, because the text is what the engine would
 * otherwise be waiting for.
 *
 * It shares the player with the other two lanes, because the speaker is one.
 */
import type { ReaderEvent } from '@shared/api'
import { player } from './audio'
import { forSpeech } from './tts'

let unsubscribe: (() => void) | null = null
let pending: {
  resolve: (stats: Record<string, unknown>) => void
  reject: (e: Error) => void
} | null = null

/** When the turn opened, so the log can say how long the first sound took. */
let openedAt = 0
let firstChunkAt = 0
let chunks = 0
let seconds = 0
let lastSentence = -1

function onEvent(ev: ReaderEvent): void {
  if (ev.type === 'audio') {
    if (ev.pcm !== undefined && ev.pcm.length > 0) {
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

/** Open a stream. False when the engine is not ready or one is already open. */
export async function begin(): Promise<boolean> {
  ensureSubscribed()
  player.stop()
  player.prime(PRIME_SECONDS)
  openedAt = Date.now()
  firstChunkAt = 0
  chunks = 0
  seconds = 0
  lastSentence = -1
  unsent = ''

  const ok = await window.yoai.reader.begin()
  if (!ok) {
    console.warn('[reader] could not begin')
  }
  return ok
}

/**
 * Text the engine has not been given yet, because it is not a whole sentence.
 *
 * The core does the real cutting -- min_chars, max_chars, the tail rule -- and
 * this is not a second copy of that. It exists because of what has to happen
 * BEFORE the cutting: the reply is markdown, and the engine must be given the
 * words without it, or it reads the asterisks out loud. Stripping cannot be
 * done a token at a time -- `**bold**` arrives in pieces and its closing marker
 * may be three deltas away -- so the text is held until a sentence has ended
 * and cleaned in one go.
 */
let unsent = ''

/**
 * A point the buffer can safely be cut and cleaned at.
 *
 * **Comma-level, not sentence-level**, and that is a correction. Cutting only
 * at full stops looked right and was not: a reply whose middle is all commas --
 * which is ordinary Chinese -- has no terminator between its first sentence and
 * its last, so the whole of it was held here and handed over in one go when the
 * model finished. Measured, the only piece of a 65-character reply was
 * dispatched two milliseconds after the turn ended, and the first word came
 * 5.28 s in.
 *
 * The cut this buffer makes is NOT the cut that matters: the core decides where
 * pieces begin and end, with min_chars and max_chars, and it cannot decide
 * about text it has not been given. All this has to do is hand text over often
 * enough for the core to have the choice.
 *
 * What it trades is markdown spanning one of these. `**说了，又说**` gets
 * stripped in two halves and loses its markers unevenly, which at worst reads
 * an asterisk aloud. Holding a whole sentence to avoid that cost seconds.
 */
const ENDS = /[。！？；…!?;\n，、,：:]/

/**
 * Past this many characters with nothing to cut at, hand it over anyway.
 *
 * Low, for the same reason: the core cannot place a cut in text it does not
 * have. A run this long with no punctuation at all is not a sentence anybody is
 * waiting to hear the end of.
 */
const FLUSH_AT = 24

function flush(force: boolean): void {
  if (unsent.length === 0) {
    return
  }
  if (!force && !ENDS.test(unsent) && unsent.length < FLUSH_AT) {
    return
  }
  const raw = unsent
  unsent = ''

  // forSpeech is the speech lane's cleaner and it assumes it is being handed a
  // WHOLE reply. One of its rules -- drop a comma at the start or end of a line
  // -- exists for markdown table rows, and on a fragment that was cut AT a
  // comma it silently eats the comma instead. Measured, that stripped every
  // comma out of a reply: 45 characters of Chinese arrived at the engine with
  // no punctuation between the first word and the full stop, which also left
  // the core's max_chars rule with nothing to fall back on.
  //
  // Restoring it here rather than changing that rule, because the rule is right
  // for the lane it was written for and this is the only caller handing it
  // fragments.
  const tail = /[，、,：:]$/.exec(raw)
  let spoken = forSpeech(raw)
  if (tail !== null && spoken.length > 0 && !/[，、,：:]$/.test(spoken)) {
    spoken += tail[0]
  }
  if (spoken.length > 0) {
    // Logged here and not only in the core: that log goes through printf to a
    // Windows console whose code page is not UTF-8, so Chinese comes out as
    // mojibake. The bytes are right, the terminal is not. This is the readable
    // copy, and it is what the engine is actually given.
    console.log(`[reader] fed ${spoken.length} chars: ${JSON.stringify(spoken)}`)
    void window.yoai.reader.feed(spoken)
  }
}

/**
 * Add a delta. Fire and forget.
 *
 * Not awaited by the caller: this runs once per token, and an IPC round trip
 * per token would put the page's own rendering behind the main process.
 */
export function feed(delta: string): void {
  if (delta.length === 0) {
    return
  }
  unsent += delta
  flush(false)
}

/** No more text. Resolves with the timings once the last sample has played. */
export function finish(): Promise<Record<string, unknown>> {
  flush(true)
  const done = new Promise<Record<string, unknown>>((resolve, reject) => {
    pending = { resolve, reject }
  })
  void window.yoai.reader.finish()
  return done
}
