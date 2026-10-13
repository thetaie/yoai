/**
 * Playing the PCM speech output produces.
 *
 * A port of examples/desktop/src/renderer/src/audio.ts, and its two rules carry
 * over unchanged because they are about the producer, not the platform:
 *
 *   - PRIME: playback does not begin until there is this much audio in hand,
 *     counted in seconds of audio rather than of clock, however the engine
 *     chose to cut its chunks.
 *   - REALTIME_MARGIN: a producer slower than real time is not streamed at
 *     all. Its audio is gathered and judged again as each block arrives; one
 *     that never gets ahead is played end to end at flush() -- later, but in
 *     one piece. On a phone this is the common case: the 1.7B talker measured
 *     3.15-3.64 RTF on a Kirin 9020 (README.md), so a reply read as it is
 *     written is gathered and then played, rather than played with a hole after
 *     every block.
 *
 * What differs is the platform. The desktop schedules buffers on an
 * AudioContext; here an AudioRenderer asks for samples when it wants them
 * ('writeData'), so this is a queue it pulls from. A hole is therefore seen as
 * it happens -- the renderer asks and the queue is short -- and the ramp that
 * stops it clicking goes on the samples written just before it.
 *
 * The callback runs on the ArkTS thread, so a screen busy for longer than the
 * renderer's buffer is heard as a gap.
 *
 * 16-bit mono at 24 kHz throughout: what the engine produces, what a recorded
 * reference voice is, and what the renderer is opened at. The samples do not
 * carry the rate; it is stated here and in napi_init.cpp.
 */
import { audio } from '@kit.AudioKit';
import { BusinessError } from '@kit.BasicServicesKit';
import hilog from '@ohos.hilog';

export const SPEECH_RATE: number = 24000;

/** AppStorage key: whether there is speech to stop. Pages read it to offer 停止. */
export const SPEECH_ACTIVE: string = 'speechActive';

/** Seconds of audio to have in hand before the first sample plays. */
const PRIME_SECONDS: number = 0.6;

/** How much faster than real time a producer must be before it is streamed. */
const REALTIME_MARGIN: number = 1.1;

/**
 * How much has to have arrived before the rate above means anything. The first
 * block arrives with whatever the producer had ready, at no cost in clock, so
 * it is left out of the estimate entirely.
 */
const JUDGE_BLOCKS: number = 2;
const JUDGE_SECONDS: number = 0.25;

/**
 * The ramp put on both sides of a hole: 4ms, short enough not to be heard as
 * a dip and long enough that the step to silence becomes a ramp. Never applied
 * at an ordinary chunk boundary, which is sample-exact.
 */
const EDGE: number = Math.round(0.004 * SPEECH_RATE);

function log(msg: string): void {
  hilog.info(0x0000, 'yoai', '%{public}s', msg);
}

export class SpeechPlayer {
  private renderer: audio.AudioRenderer | null = null;
  private creating: Promise<audio.AudioRenderer | null> | null = null;
  /** The renderer has been started and not yet stopped. */
  private running: boolean = false;
  /** The last stop, which a start waits for: a renderer still stopping refuses to start. */
  private stopping: Promise<void> = Promise.resolve();

  /** Chunks gathered before playback starts; see PRIME_SECONDS. */
  private pending: Int16Array[] = [];
  private pendingSeconds: number = 0;
  /** The verdict is in and the chunks go to `queue` as they arrive. */
  private started: boolean = false;
  /** flush() was called: whatever is queued is the end of the stream. */
  private ended: boolean = false;
  /** Set by expect(), cleared when the stream ends. */
  private expected: boolean = false;
  private primeSeconds: number = PRIME_SECONDS;

  /** What the renderer pulls from, and how far into the first chunk it has read. */
  private queue: Int16Array[] = [];
  private head: number = 0;
  /** The last samples out were ramped down into a hole; ramp the next ones up. */
  private fadeInNext: boolean = false;

  /** How the producer is doing, for the decision REALTIME_MARGIN describes. */
  private firstPushAt: number = 0;
  private firstBlockSeconds: number = 0;
  private blocksIn: number = 0;
  private totalSeconds: number = 0;
  private underruns: number = 0;
  private verdict: string = '';

  private watchers: Set<() => void> = new Set<() => void>();
  private wasActive: boolean = false;

  /** Queue one chunk of 16-bit mono samples at 24 kHz. */
  push(pcm: Int16Array): void {
    if (pcm.length === 0) {
      return;
    }
    const seconds: number = pcm.length / SPEECH_RATE;
    this.totalSeconds += seconds;
    this.blocksIn++;
    if (this.blocksIn === 1) {
      this.firstPushAt = Date.now();
      this.firstBlockSeconds = seconds;
    }
    if (this.started) {
      this.queue.push(pcm);
      this.play();
      this.notify();
      return;
    }

    this.pending.push(pcm);
    this.pendingSeconds += seconds;
    this.notify();
    if (this.pendingSeconds < this.primeSeconds) {
      return;
    }
    const rate: number = this.producerRate();
    if (rate < 0) {
      return;
    }
    // Judged again on every block while holding: a reply read as one generation
    // starts slow, waiting on the language model's text, and then outruns
    // playback. One slower than playback throughout is held as before.
    const next: string = rate >= REALTIME_MARGIN ? 'stream' : 'hold';
    if (next !== this.verdict) {
      log('[audio] producer at ' + rate.toFixed(2) + 'x real time -> ' +
        (next === 'stream'
          ? 'playing now, ' + this.pendingSeconds.toFixed(2) + ' s in hand'
          : 'slower than playback, holding for now'));
    }
    this.verdict = next;
    if (next === 'stream') {
      this.begin();
    }
  }

  /** No more chunks are coming; play whatever is still being gathered. */
  flush(): void {
    if (!this.started && this.pending.length > 0) {
      log('[audio] playing ' + this.pendingSeconds.toFixed(1) + ' s in one piece, gathered over ' +
        ((Date.now() - this.firstPushAt) / 1000).toFixed(1) + ' s in ' + this.blocksIn + ' block(s)');
      this.begin();
    } else if (this.started) {
      const elapsed: number = (Date.now() - this.firstPushAt) / 1000;
      log('[audio] streamed ' + this.totalSeconds.toFixed(1) + ' s in ' + this.blocksIn +
        ' block(s) over ' + elapsed.toFixed(1) + ' s (' + (this.totalSeconds / elapsed).toFixed(2) +
        'x sustained), ' + this.underruns + ' underrun(s)');
    }
    // This stream is over; what it queued still plays out. The next one is
    // judged on its own and may begin while this one is still audible.
    this.ended = true;
    this.started = false;
    this.expected = false;
    this.resetJudgement();
    if (this.queue.length === 0) {
      this.halt();
    }
    this.notify();
  }

  /** Drop everything queued and stop now. */
  stop(): void {
    this.pending = [];
    this.pendingSeconds = 0;
    this.queue = [];
    this.head = 0;
    this.started = false;
    this.ended = false;
    this.expected = false;
    this.fadeInNext = false;
    this.resetJudgement();
    this.halt();
    this.notify();
  }

  /**
   * A stream is starting: count it as active from now, before its first chunk
   * arrives, until flush() or stop(). So there is something to stop from the
   * moment speech is asked for.
   */
  expect(): void {
    this.expected = true;
    this.ended = false;
    this.notify();
  }

  /**
   * Whether there is speech to stop: a stream expected and not yet flushed,
   * audio gathered or queued, or the renderer still playing. Stays true after
   * the engine is done for as long as the speaker still has what it made.
   */
  active(): boolean {
    return this.expected || this.pending.length > 0 || this.queue.length > 0 || this.running;
  }

  /** Call `listener` whenever active() changes. Returns the unsubscribe function. */
  watch(listener: () => void): () => void {
    this.watchers.add(listener);
    return () => {
      this.watchers.delete(listener);
    };
  }

  /**
   * Seconds of audio this stream should have in hand before it starts. Set
   * before the stream begins; it goes back to the default when the stream ends.
   */
  prime(seconds: number): void {
    this.primeSeconds = Number.isFinite(seconds) && seconds > 0 ? seconds : PRIME_SECONDS;
  }

  //----------------------------------------------------------------------------

  private notify(): void {
    const now: boolean = this.active();
    if (now === this.wasActive) {
      return;
    }
    this.wasActive = now;
    AppStorage.setOrCreate(SPEECH_ACTIVE, now);
    this.watchers.forEach((w: () => void) => {
      w();
    });
  }

  /** One stream is judged on its own: the next may come from another lane. */
  private resetJudgement(): void {
    this.firstPushAt = 0;
    this.firstBlockSeconds = 0;
    this.blocksIn = 0;
    this.totalSeconds = 0;
    this.underruns = 0;
    this.verdict = '';
    this.primeSeconds = PRIME_SECONDS;
  }

  /** Seconds of audio produced per second of clock, excluding the first block; -1 too early. */
  private producerRate(): number {
    const elapsed: number = (Date.now() - this.firstPushAt) / 1000;
    if (this.blocksIn < JUDGE_BLOCKS || elapsed < JUDGE_SECONDS) {
      return -1;
    }
    return (this.pendingSeconds - this.firstBlockSeconds) / elapsed;
  }

  /**
   * Begin, with everything gathered so far laid end to end -- after whatever
   * an earlier stream left queued, which is why `ended` goes: the queue running
   * dry from here on is a hole in this stream, not its end.
   */
  private begin(): void {
    this.started = true;
    this.ended = false;
    for (const p of this.pending) {
      this.queue.push(p);
    }
    this.pending = [];
    this.pendingSeconds = 0;
    this.play();
  }

  /** Start the renderer if it is not running; it pulls from `queue` from then on. */
  private play(): void {
    if (this.running) {
      return;
    }
    this.running = true;
    const stopping: Promise<void> = this.stopping;
    this.ensure().then(async (r: audio.AudioRenderer | null) => {
      if (r === null) {
        this.running = false;
        this.queue = [];
        this.notify();
        return;
      }
      await stopping;
      // Stopped or drained while the renderer was being made.
      if (!this.running) {
        return;
      }
      try {
        await r.start();
      } catch (err) {
        const e = err as BusinessError;
        log('[audio] the renderer would not start: ' + e.code + ' ' + e.message);
        this.running = false;
        this.notify();
      }
    });
  }

  /** Stop the renderer. The queue is the caller's business. */
  private halt(): void {
    if (!this.running) {
      return;
    }
    this.running = false;
    const r: audio.AudioRenderer | null = this.renderer;
    if (r !== null) {
      this.stopping = r.stop().catch((err: BusinessError) => {
        // Not started yet, or already stopped: either way it is quiet.
        log('[audio] stop: ' + err.code + ' ' + err.message);
      });
    }
  }

  /** The one renderer, made on first use and kept: making one costs a beat. */
  private ensure(): Promise<audio.AudioRenderer | null> {
    if (this.renderer !== null) {
      return Promise.resolve(this.renderer);
    }
    if (this.creating !== null) {
      return this.creating;
    }
    const options: audio.AudioRendererOptions = {
      streamInfo: {
        samplingRate: audio.AudioSamplingRate.SAMPLE_RATE_24000,
        channels: audio.AudioChannel.CHANNEL_1,
        sampleFormat: audio.AudioSampleFormat.SAMPLE_FORMAT_S16LE,
        encodingType: audio.AudioEncodingType.ENCODING_TYPE_RAW
      },
      rendererInfo: {
        usage: audio.StreamUsage.STREAM_USAGE_VOICE_ASSISTANT,
        rendererFlags: 0
      }
    };
    this.creating = audio.createAudioRenderer(options).then((r: audio.AudioRenderer) => {
      r.on('writeData', (buffer: ArrayBuffer) => this.fill(buffer));
      this.renderer = r;
      this.creating = null;
      return r;
    }).catch((err: BusinessError) => {
      log('[audio] no renderer: ' + err.code + ' ' + err.message);
      this.creating = null;
      return null;
    });
    return this.creating;
  }

  /** The renderer wants `buffer` filled. */
  private fill(buffer: ArrayBuffer): audio.AudioDataCallbackResult {
    const out: Int16Array = new Int16Array(buffer, 0, Math.floor(buffer.byteLength / 2));
    let at: number = 0;
    const from: number = at;
    while (at < out.length && this.queue.length > 0) {
      const chunk: Int16Array = this.queue[0];
      const n: number = Math.min(out.length - at, chunk.length - this.head);
      out.set(chunk.subarray(this.head, this.head + n), at);
      at += n;
      this.head += n;
      if (this.head >= chunk.length) {
        this.queue.shift();
        this.head = 0;
      }
    }
    if (this.fadeInNext && at > from) {
      const edge: number = Math.min(EDGE, at - from);
      for (let i = 0; i < edge; i++) {
        out[from + i] = Math.round(out[from + i] * (i / edge));
      }
      this.fadeInNext = false;
    }
    if (at < out.length) {
      out.fill(0, at);
      if (this.ended && this.queue.length === 0) {
        // The stream is over and all of it has been handed to the renderer.
        // Stopped from outside this callback, which must return first.
        setTimeout(() => {
          if (this.ended && this.queue.length === 0) {
            this.halt();
            this.notify();
          }
        }, 0);
      } else if (at > from && !this.fadeInNext) {
        // A hole opens here: the producer fell behind. Ramp what was written
        // down to the silence that follows, and the next audio up out of it.
        const edge: number = Math.min(EDGE, at - from);
        for (let i = 0; i < edge; i++) {
          out[at - 1 - i] = Math.round(out[at - 1 - i] * (i / edge));
        }
        this.fadeInNext = true;
        this.underruns++;
        log('[audio] underrun #' + this.underruns + ' after block ' + this.blocksIn);
      }
    }
    return audio.AudioDataCallbackResult.VALID;
  }
}

/**
 * The one player, shared by every lane that speaks and by a recording played
 * back: the speaker is one, and two renderers could talk over each other.
 * Whoever starts speaking stops whoever was.
 */
export const player: SpeechPlayer = new SpeechPlayer();
