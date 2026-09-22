/**
 * Playing the PCM speech output produces.
 *
 * The page owns this because only the page has an AudioContext. Chunks arrive
 * over IPC as they are generated, so this is a scheduler rather than a player:
 * each chunk is queued to start exactly where the previous one ended, and the
 * gap that would otherwise open between two `start()` calls -- the one that
 * sounds like a click in the middle of a word -- never happens.
 *
 * Rate is 24 kHz mono, stated in shared/api.ts and native/src/binding.cpp and
 * nowhere else; the samples do not carry it.
 */

/** What the engine produces, and what the context is asked to run at. */
const SAMPLE_RATE = 24000

/**
 * Seconds of audio to have IN HAND before the first sample plays.
 *
 * A duration of audio, not a delay in wall-clock time, and that is the whole
 * point. This began as a fixed lead-in of 0.12 s and then 0.35 s, and both were
 * guesses at the same unknown: how much audio the first chunk actually
 * contains. Measured, the engine emits its first chunk about 160 ms in and that
 * chunk is SHORT -- it is whatever was ready, not a fixed slice -- so a cushion
 * counted in seconds of clock ran out before the second chunk arrived and left
 * a 50-60 ms hole in the speech.
 *
 * Counting the audio instead makes the guarantee the one that matters:
 * playback does not begin until there is this much to play, however the engine
 * chose to cut it. At RTF 0.56 the engine then gains about half a second of
 * lead per second spoken, so the cushion only ever has to cover the beginning.
 */
const PRIME_SECONDS = 0.6

/**
 * How far ahead to schedule when nothing is already queued.
 *
 * Only for the first chunk of a stream and for recovering after an underrun:
 * scheduling at exactly `currentTime` is scheduling in the past by the time the
 * audio thread looks at it.
 */
const GUARD = 0.05

export interface Player {
  /** Queue one chunk of mono 24 kHz samples. */
  push(pcm: Float32Array): void
  /** No more chunks are coming; play whatever is still being gathered. */
  flush(): void
  /** Drop everything queued and stop immediately. */
  stop(): void
  /** Whether anything is scheduled and has not finished. */
  playing(): boolean
}

export function createPlayer(): Player {
  let ctx: AudioContext | null = null
  /** Where the next chunk starts. 0 means nothing is playing. */
  let nextAt = 0
  let live: AudioBufferSourceNode[] = []
  let underruns = 0

  /** Chunks gathered before playback starts; see PRIME_SECONDS. */
  let pending: Float32Array[] = []
  let pendingSeconds = 0
  let started = false

  const ensure = (): AudioContext => {
    if (ctx === null) {
      // At the ENGINE's rate, not the device's. A 24 kHz AudioBuffer in a
      // 48 kHz context is resampled per buffer, and each buffer's resampler
      // starts cold -- so every one of the twenty-odd chunks in a reply meets
      // its neighbour across a discontinuity. Asking for the context at 24 kHz
      // moves that to a single resample on the way to the device, where it
      // belongs, and the chunk boundaries become exact.
      ctx = new AudioContext({ sampleRate: SAMPLE_RATE })
    }
    // Chromium starts an AudioContext SUSPENDED until the page has seen a user
    // gesture, and `resume()` before one is refused. A speak button is a click
    // and is fine; reading a reply automatically is not a gesture at all, so
    // the context can be suspended at exactly the moment the first chunk
    // arrives. A suspended context does not advance currentTime, so everything
    // scheduled against it waits -- which looks like playback holding back for
    // the whole synthesis, and is not.
    if (ctx.state === 'suspended') {
      void ctx.resume()
    }
    return ctx
  }

  /** One listener, removed as soon as it fires; the first gesture is enough. */
  const unlock = (): void => {
    ensure()
  }
  for (const ev of ['pointerdown', 'keydown', 'touchstart']) {
    window.addEventListener(ev, unlock, { once: true, capture: true })
  }

  const schedule = (pcm: Float32Array): void => {
    const c = ensure()

    const buf = c.createBuffer(1, pcm.length, SAMPLE_RATE)
    // getChannelData().set() rather than copyToChannel(): the samples arrive
    // over IPC as a plain Float32Array, whose buffer type TypeScript will not
    // narrow to the ArrayBuffer copyToChannel now demands. Same copy either way.
    buf.getChannelData(0).set(pcm)

    const src = c.createBufferSource()
    src.buffer = buf
    src.connect(c.destination)

    if (nextAt < c.currentTime) {
      // Its slot has already gone past: the speech has a hole exactly here.
      // Reported rather than silently patched, because the remedy is a longer
      // prime and the only way to know the prime is too short is to be told.
      underruns++
      console.warn(
        `[tts] audio underrun #${underruns}: chunk arrived ${(
          (c.currentTime - nextAt) *
          1000
        ).toFixed(0)} ms late`
      )
      nextAt = c.currentTime + GUARD
    }

    src.start(nextAt)
    nextAt += buf.duration

    live.push(src)
    src.onended = (): void => {
      live = live.filter((s) => s !== src)
    }
  }

  /** Begin, with everything gathered so far laid end to end. */
  const begin = (): void => {
    const c = ensure()
    started = true
    nextAt = c.currentTime + GUARD
    for (const p of pending) {
      schedule(p)
    }
    pending = []
    pendingSeconds = 0
  }

  return {
    push(pcm: Float32Array): void {
      if (pcm.length === 0) {
        return
      }
      if (started) {
        schedule(pcm)
        return
      }
      pending.push(pcm)
      pendingSeconds += pcm.length / SAMPLE_RATE
      if (pendingSeconds >= PRIME_SECONDS) {
        begin()
      }
    },

    flush(): void {
      // An utterance shorter than the prime would otherwise never be heard.
      if (!started && pending.length > 0) {
        begin()
      }
    },

    stop(): void {
      for (const s of live) {
        try {
          s.stop()
        } catch {
          // Already finished. Not a problem, and not worth distinguishing from
          // one that had not started.
        }
      }
      live = []
      pending = []
      pendingSeconds = 0
      started = false
      underruns = 0
      // 0 rather than currentTime, so the next utterance's first chunk is not
      // measured against a slot left over from this one. That is what made the
      // counter report the pause between two replies -- seconds of it -- as if
      // the speech had a hole in it.
      nextAt = 0
    },

    playing(): boolean {
      return live.length > 0
    }
  }
}
