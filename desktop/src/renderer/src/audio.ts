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
 * How much faster than real time a producer has to be before its audio is
 * played as it arrives.
 *
 * The prime above answers "is there enough to start with"; this answers "will
 * there be enough to continue", and they are different questions with different
 * answers per lane. Speech output generates at about 1.8x real time, so every
 * second played buys most of another second of lead and the cushion only ever
 * has to cover the beginning. Omni's Talker measured 0.32x -- it produces one
 * second of speech every three seconds -- and no cushion fixes that: playback
 * drains whatever is in hand and then waits, which is what fifteen underruns of
 * 1.8-4.5 s each were. The ratio itself is the gap.
 *
 * So a producer slower than real time is not streamed at all. Its audio is
 * gathered until flush(), then played end to end: later, but in one piece.
 *
 * Measured rather than configured, because the one number that decides is the
 * one this machine is actually managing right now -- the same model at the same
 * settings runs at a different ratio on a phone, and a lane that gets faster
 * starts streaming on its own.
 */
const REALTIME_MARGIN = 1.1

/**
 * How much has to have arrived before the rate above means anything.
 *
 * A rate needs two points. The first block arrives with whatever the producer
 * had ready, at no cost in clock, so it says nothing about the rate and is
 * excluded from the estimate entirely -- counting it made omni look like 0.56x
 * when it was running at 0.28x.
 */
const JUDGE_BLOCKS = 2
const JUDGE_SECONDS = 0.25

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

/**
 * The one player, shared by every lane that produces speech.
 *
 * A singleton because the speaker is one: speech output and omni both hand back
 * 24 kHz mono float, and two players would mean two AudioContexts able to talk
 * over each other. Whoever starts speaking stops whoever was.
 */
export const player: Player = createPlayer()

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

  /** How the producer is doing, for the decision REALTIME_MARGIN describes. */
  let firstPushAt = 0
  let firstBlockSeconds = 0
  let blocksIn = 0
  /** Total seconds of audio this stream carried, for the closing line. */
  let totalSeconds = 0
  /** null until the stream has been judged; then 'stream' or 'hold'. */
  let verdict: 'stream' | 'hold' | null = null

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
      // Reported rather than silently patched, because a hole means the
      // producer is losing ground and the only useful response is to know it.
      //
      // With REALTIME_MARGIN in place this should now be rare: a lane measured
      // slower than playback is never started in the first place. One here
      // means a lane that looked fast enough at the second block and then
      // slowed down, so the lateness is what to read.
      underruns++
      console.warn(
        `[audio] underrun #${underruns}: block ${blocksIn} arrived ${(
          (c.currentTime - nextAt) *
          1000
        ).toFixed(0)} ms after the speaker ran dry`
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
    // max(), not assignment: an utterance can begin while the previous one is
    // still audible -- flush() ends a stream without stopping its playback --
    // and starting this one at currentTime would lay it OVER the tail of that
    // one. stop() sets nextAt to 0, so an interrupted stream still starts now.
    nextAt = Math.max(nextAt, c.currentTime + GUARD)
    for (const p of pending) {
      schedule(p)
    }
    pending = []
    pendingSeconds = 0
  }

  /**
   * Forget this stream, keeping whatever it has already scheduled.
   *
   * One utterance is one stream, and every stream is judged on its own: the
   * producer's rate is a property of the lane that is running now, so a reply
   * spoken by omni must not leave its verdict behind for the next one spoken by
   * the speech lane. They share this player because the speaker is one.
   */
  const endStream = (): void => {
    started = false
    pending = []
    pendingSeconds = 0
    firstPushAt = 0
    firstBlockSeconds = 0
    blocksIn = 0
    totalSeconds = 0
    underruns = 0
    verdict = null
  }

  /**
   * Seconds of audio produced per second of clock, excluding the first block.
   *
   * -1 while there is not enough to say. See JUDGE_BLOCKS.
   */
  const producerRate = (): number => {
    const elapsed = (performance.now() - firstPushAt) / 1000
    if (blocksIn < JUDGE_BLOCKS || elapsed < JUDGE_SECONDS) {
      return -1
    }
    return (pendingSeconds - firstBlockSeconds) / elapsed
  }

  return {
    push(pcm: Float32Array): void {
      if (pcm.length === 0) {
        return
      }
      const seconds = pcm.length / SAMPLE_RATE
      totalSeconds += seconds
      blocksIn++
      if (blocksIn === 1) {
        firstPushAt = performance.now()
        firstBlockSeconds = seconds
      }
      if (started) {
        schedule(pcm)
        return
      }

      pending.push(pcm)
      pendingSeconds += seconds

      if (verdict === 'hold' || pendingSeconds < PRIME_SECONDS) {
        return
      }
      const rate = producerRate()
      if (rate < 0) {
        // Enough to start with, but not yet enough to know whether it can be
        // continued. Waiting for the next block costs a moment against a gap in
        // the middle of a word.
        return
      }
      verdict = rate >= REALTIME_MARGIN ? 'stream' : 'hold'
      console.log(
        `[audio] producer at ${rate.toFixed(2)}x real time -> ` +
          (verdict === 'stream'
            ? `playing now, ${pendingSeconds.toFixed(2)} s in hand`
            : 'slower than playback, holding until the whole reply is in')
      )
      if (verdict === 'stream') {
        begin()
      }
    },

    flush(): void {
      // Everything held, and anything shorter than the prime, which would
      // otherwise never be heard at all.
      if (!started && pending.length > 0) {
        const waited = ((performance.now() - firstPushAt) / 1000).toFixed(1)
        console.log(
          `[audio] playing ${pendingSeconds.toFixed(1)} s in one piece, ` +
            `gathered over ${waited} s in ${blocksIn} block(s)`
        )
        begin()
      } else if (started) {
        console.log(
          `[audio] streamed ${totalSeconds.toFixed(1)} s in ${blocksIn} block(s), ` +
            `${underruns} underrun(s)`
        )
      }
      endStream()
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
      endStream()
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
