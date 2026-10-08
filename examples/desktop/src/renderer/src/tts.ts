/**
 * Speaking text that already exists, from the page's side: a message's 🔊 and
 * the settings screen's test.
 *
 * Through the reading lane, which is the only one the desktop speaks through:
 * the text is one stream, fed in one go and finished at once. The core cuts it
 * into pieces anchored to one reference voice, exactly as it does a reply being
 * written. It used to go through a lane of its own that spoke the whole text as
 * one utterance; that lane needed a second copy of the weights, and it could
 * not keep a voice steady when a reply WAS cut into pieces.
 *
 * A module singleton rather than a hook, because what it fronts is one: the
 * core reads one stream at a time, so a message's button and a reply being read
 * cannot both speak -- starting one stops the other.
 */
import * as reader from './reader'

/** Which message is being spoken, '' when nothing is. Drives the buttons. */
let speakingId = ''
/** Why the last utterance stopped early, '' when it did not. */
let lastError = ''
const watchers = new Set<() => void>()

function announce(): void {
  for (const w of watchers) {
    w()
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
 * Speak `text`, tagging it with `id` so a button can show its own state.
 * Resolves to '' when it started, or to a reason it did not.
 *
 * The text as shown, markdown and emoji included: the core turns it into what
 * is spoken (src/voice/text/speakable_stream.hpp). Asking again while something is
 * speaking STOPS the old one and starts this, which is what a second click on a
 * different message means.
 */
export async function speak(id: string, text: string): Promise<string> {
  if (text.trim().length === 0) {
    return 'nothing to say'
  }
  if (!(await reader.loaded())) {
    return 'the voice is not loaded'
  }
  await stop()

  // Set before the await: audio can start arriving the moment the stream opens,
  // and a button that has not yet flipped would look unresponsive.
  speakingId = id
  lastError = ''
  announce()

  if (!(await reader.begin())) {
    speakingId = ''
    lastError = 'the voice could not start'
    announce()
    return lastError
  }
  reader.feed(text)
  // Finished GENERATING, not playing: the player keeps what it was given.
  void reader.finish().then(
    () => settle(id, ''),
    (e: Error) => settle(id, e.message.length > 0 ? e.message : '朗读失败')
  )
  return ''
}

function settle(id: string, error: string): void {
  if (speakingId !== id) {
    return   // stopped, or replaced by another message
  }
  speakingId = ''
  lastError = error
  if (error.length > 0) {
    console.error('[tts]', error)
  }
  announce()
}

/** Stop the engine and the playback. Safe to call when nothing is speaking. */
export async function stop(): Promise<void> {
  const was = speakingId
  speakingId = ''
  if (was.length > 0) {
    announce()
  }
  await reader.cancel()
}

/**
 * Resolve when the current utterance finishes, with its error or ''.
 *
 * For callers that started one and want to report a failure that arrives
 * later: speak() returns as soon as the stream opens.
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
