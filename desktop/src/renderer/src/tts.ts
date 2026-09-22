/**
 * Speaking, from the page's side.
 *
 * A module singleton rather than a hook, because the thing it fronts is a
 * singleton: the core keeps ONE Qwen3-TTS instance behind a mutex, so two
 * components asking to speak are two requests for the same engine, not two
 * engines. The settings screen's test button, a message's speak button and the
 * automatic reading of a reply all go through here and cannot get out of step.
 *
 * Audio arrives in chunks and is handed straight to the player, which schedules
 * each one where the last ended. Nothing is accumulated: by the time the last
 * chunk arrives the first has already been heard.
 *
 * **One utterance per reply, and that is not a simplification.** Splitting a
 * reply into sentences makes Qwen3-TTS synthesise several INDEPENDENT pieces of
 * speech, each with its own prosody arc and its own start and stop; played back
 * to back they are audibly stitched, and no choice of chunk size fixes it,
 * because the seam is what a chunk boundary IS.
 *
 * It was worth doing only if the engine were slower than playback. Measured on
 * this machine with the CustomVoice 1.7B Q4 talker and the 12 Hz tokenizer:
 * **RTF 0.56, first audio 0.13 s, at 2% into the generation, over 20-29
 * streaming chunks.** The engine outruns the ear from almost the first moment,
 * so one continuous generation plays gap-free -- the reason to chunk never
 * existed on this configuration.
 */
import type { TtsEvent } from '@shared/api'
import { player } from './audio'

/** Which message is being spoken, '' when nothing is. Drives the buttons. */
let speakingId = ''
/**
 * Why the last utterance stopped early, '' when it did not.
 *
 * Kept rather than only logged, because the failure that matters most arrives
 * here and nowhere else: a voice_design checkpoint with no instruct loads
 * perfectly and then refuses to speak, so the only sign is this string.
 */
let lastError = ''
let unsubscribe: (() => void) | null = null
const watchers = new Set<() => void>()

/** Sentences waiting for the engine, and the id they belong to. */
let queue: string[] = []
let pumping = false
/** Resolves when the utterance in flight ends, however it ends. */
let endOfUtterance: (() => void) | null = null

function announce(): void {
  for (const w of watchers) {
    w()
  }
}

/** When the current utterance was handed to the engine, for the timing below. */
let saidAt = 0
let firstChunkAt = 0

function onEvent(ev: TtsEvent): void {
  if (ev.type === 'audio') {
    if (ev.pcm !== undefined) {
      // The one number that says whether playback is really streaming: how long
      // after asking did the FIRST samples reach the speaker. The engine's own
      // firstAudioSeconds measures the same thing on its side, so a gap between
      // the two is the IPC and this module, and a matching pair means the wait
      // is synthesis and nothing here can shorten it.
      if (firstChunkAt === 0) {
        firstChunkAt = Date.now()
        console.log(`[tts] first audio at the speaker: ${firstChunkAt - saidAt} ms`)
      }
      player.push(ev.pcm)
    }
    return
  }
  // 'end' and 'error' both finish the utterance. The stats on 'end' are worth
  // having in the console while this lane is young -- realtime, chunks and
  // firstAudioSeconds are exactly what design/voice.md is measuring.
  if (ev.type === 'end' && ev.stats !== undefined && ev.stats.length > 0) {
    console.log('[tts]', ev.stats)
  }
  // No more chunks for this utterance: release anything still being gathered
  // for the prime, which an utterance shorter than it would otherwise hold
  // forever.
  player.flush()
  if (ev.type === 'error') {
    lastError = ev.error !== undefined && ev.error.length > 0 ? ev.error : '朗读失败'
    console.error('[tts]', lastError)
    // A failing utterance takes the rest of the reply with it. Whatever is
    // wrong -- no instruct, a bad checkpoint -- the next sentence would fail
    // the same way, and a queue draining one error per sentence is just noise.
    queue = []
  }
  const done = endOfUtterance
  endOfUtterance = null
  if (done !== null) {
    done()
  }
}

function ensureSubscribed(): void {
  if (unsubscribe === null) {
    unsubscribe = window.bodyclock.tts.onEvent(onEvent)
  }
}

/** Subscribe to "who is speaking" changes. Returns the unsubscribe function. */
export function watch(fn: () => void): () => void {
  watchers.add(fn)
  return () => {
    watchers.delete(fn)
  }
}

/** '' when nothing is being spoken, else the id passed to speak(). */
export function speakingOf(): string {
  return speakingId
}

/** Why the last utterance stopped early, '' when it did not. */
export function errorOf(): string {
  return lastError
}

/**
 * Speak one utterance and resolve when it has finished generating.
 *
 * Finished GENERATING, not finished playing: the player keeps scheduling what
 * it has been given, so starting the next sentence's synthesis while the
 * previous one is still audible is exactly what keeps the speech continuous.
 */
async function speakOne(text: string): Promise<void> {
  saidAt = Date.now()
  firstChunkAt = 0
  const started = await window.bodyclock.tts.say(text)
  if (!started) {
    return
  }
  await new Promise<void>((resolve) => {
    endOfUtterance = resolve
  })
}

async function pump(): Promise<void> {
  if (pumping) {
    return
  }
  pumping = true
  try {
    while (queue.length > 0) {
      const next = queue.shift()
      if (next !== undefined) {
        await speakOne(next)
      }
    }
  } finally {
    pumping = false
    speakingId = ''
    announce()
  }
}

/**
 * Everything that is written but not said.
 *
 * `\p{Extended_Pictographic}` is the emoji core, and it is used instead of the
 * obvious `\p{Emoji}` because that property ALSO matches the ASCII digits, `#`
 * and `*` -- a reply containing "14 tok/s" would lose its number. The rest are
 * the pieces that join pictographs into one glyph: the variation selectors, the
 * zero-width joiner, the skin-tone modifiers, the regional indicators behind
 * flags, and the enclosing keycap. A keycap's DIGIT is deliberately left
 * behind, so a numbered item is still read as its number.
 */
const NOT_SPOKEN =
  /[\p{Extended_Pictographic}\p{Emoji_Modifier}\p{Regional_Indicator}\uFE0E\uFE0F\u200D\u20E3]/gu

/**
 * Markdown is written for the eye, and the engine reads it out loud.
 *
 * A reply containing `**榨菜**` was spoken with the asterisks in it, and a
 * bulleted list read its bullets. None of this is decoration to a text-to-speech
 * model -- it is characters, and it says them.
 *
 * What each rule KEEPS is the point: the text survives and the markup goes. A
 * link keeps its label and loses its URL, which is the worst thing on this list
 * to hear read out. A heading keeps its words and loses its hashes.
 *
 * Underscores are deliberately NOT treated as emphasis. `Q4_K_M` and
 * `sensevoice_small` are ordinary words here, and an italic rule that ate them
 * would turn a model name into "Q4KM". Doubled `__bold__` is still stripped,
 * because a single underscore can never match it.
 */
function stripMarkdown(text: string): string {
  return (
    text
      // The fence lines only. The code between them is still words, and
      // dropping it silently would be worse than reading it.
      .replace(/^\s*```.*$/gm, '')
      .replace(/!\[([^\]]*)\]\([^)]*\)/g, '$1')
      .replace(/\[([^\]]+)\]\([^)]*\)/g, '$1')
      .replace(/^\s{0,3}#{1,6}\s+/gm, '')
      .replace(/^\s{0,3}>\s?/gm, '')
      // A rule is three or more of the same mark alone on a line.
      .replace(/^\s{0,3}([-*_])(?:\s*\1){2,}\s*$/gm, '')
      // A table's separator row is punctuation all the way across. The pipes in
      // the rows that remain become pauses rather than spoken characters.
      .replace(/^\s*\|?[\s:|-]*-[\s:|-]*\|?\s*$/gm, '')
      .replace(/\s*\|\s*/g, '，')
      // A row's outer pipes become a comma at each end and its empty cells
      // become doubled ones, which the engine reads as extra pauses.
      .replace(/，{2,}/g, '，')
      .replace(/^，|，$/gm, '')
      .replace(/^\s*[-*+]\s+/gm, '')
      .replace(/(\*\*|__)(.+?)\1/g, '$2')
      .replace(/\*(?=\S)([^*]+?)(?<=\S)\*/g, '$1')
      .replace(/~~(.+?)~~/g, '$1')
      .replace(/`([^`]*)`/g, '$1')
      .replace(/\n{3,}/g, '\n\n')
  )
}

/**
 * The text as it should be SPOKEN, which is not the text as it is shown. The
 * transcript keeps its emoji and its formatting; only what reaches the engine
 * loses them.
 */
function forSpeech(text: string): string {
  return stripMarkdown(text)
    .replace(NOT_SPOKEN, '')
    .replace(/[ \t]{2,}/g, ' ')
    .trim()
}

/**
 * Speak `text` as one utterance, tagging it with `id` so a button can show its
 * own state. Resolves to '' when it started, or to a reason it did not.
 *
 * Asking again while something is speaking STOPS the old one and starts this,
 * which is what a second click on a different message means.
 */
export async function speak(id: string, text: string): Promise<string> {
  const trimmed = forSpeech(text)
  if (trimmed.length === 0) {
    return 'nothing to say'
  }
  ensureSubscribed()

  if (speakingId.length > 0 || queue.length > 0) {
    await stop()
  }

  // Set before the await: chunks can start arriving the moment the main process
  // returns, and a button that has not yet flipped would look unresponsive.
  speakingId = id
  lastError = ''
  announce()

  queue.push(trimmed)
  void pump()
  return ''
}

/** Stop the engine and the playback. Safe to call when nothing is speaking. */
export async function stop(): Promise<void> {
  queue = []
  player.stop()
  speakingId = ''
  announce()
  await window.bodyclock.tts.cancel()
  // Release a pump waiting on an utterance the engine has just been told to
  // drop; without this it would sit there until the cancel produced an event.
  const done = endOfUtterance
  endOfUtterance = null
  if (done !== null) {
    done()
  }
}

/**
 * Resolve when the current utterance finishes, with its error or ''.
 *
 * For callers that started a say and want to report a failure that arrives
 * later: say() returns as soon as the engine accepts the text, so a refusal --
 * a voice_design checkpoint with no instruct, most of all -- lands on the event
 * stream well after the call resolved.
 */
export function watchOnce(): Promise<string> {
  return new Promise<string>((resolve) => {
    const off = watch(() => {
      if (speakingId.length === 0) {
        off()
        resolve(lastError)
      }
    })
  })
}
