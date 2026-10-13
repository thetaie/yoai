/**
 * Microphone to 16-bit mono PCM at 16 kHz, which is exactly what the core takes
 * for analysis -- or at 24 kHz, which is what it takes for a reference voice.
 *
 * The capturer is opened at the rate the core needs rather than at whatever
 * the device prefers, so nothing here resamples. That is deliberate and it is
 * worth knowing why: the core carries no resampler, and a resampler that differs
 * from the one a measurement was taken with changes the answer quietly. This
 * project has already spent an afternoon on a one-character transcription
 * difference that turned out to be exactly that.
 *
 * SOURCE_TYPE_MIC rather than the voice-communication source, which would add
 * echo cancellation and noise suppression: a reference voice is a description
 * of how someone sounds, and it should be the unprocessed sound.
 *
 * Chunks are copied out of the framework's buffer as they arrive. The buffer
 * handed to 'readData' belongs to the audio stack and is reused, so holding a
 * reference to it would give you the newest audio in every slot by the time
 * recording stopped.
 */
import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import { audio } from '@kit.AudioKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const TAG: string = 'yoai-voice';

function log(msg: string): void {
  hilog.info(0x3200, TAG, '%{public}s', msg);
}

export const VOICE_SAMPLE_RATE: number = 16000;

/** Below this the within-recording comparison has nothing to compare. */
export const VOICE_MIN_SECONDS: number = 8;

/** The rate a reference voice is recorded at: what the speech engine takes. */
export const REFERENCE_SAMPLE_RATE: number = 24000;

export class VoiceRecorder {
  private capturer: audio.AudioCapturer | undefined = undefined;
  private chunks: Uint8Array[] = [];
  private bytes: number = 0;
  private rate: number;
  private lastLevel: number = 0;
  private loudest: number = 0;

  /** 16000 for analysis, 24000 for a reference voice. */
  constructor(rate: number = VOICE_SAMPLE_RATE) {
    this.rate = rate;
  }

  /** Whether the capturer is open and taking audio right now. */
  recording(): boolean {
    return this.capturer !== undefined;
  }

  /** Seconds captured so far, for a live read-out while recording. */
  seconds(): number {
    return this.bytes / 2 / this.rate;
  }

  /** The level of the last block, 0..1 RMS, for a meter. */
  level(): number {
    return this.lastLevel;
  }

  /** The loudest sample so far, 0..1; at 1 the take is clipped. */
  peak(): number {
    return this.loudest;
  }

  /**
   * Ask once for the microphone. Returns false when the user says no, which is
   * a normal answer and not an error to report as one.
   */
  static async requestPermission(ctx: common.UIAbilityContext): Promise<boolean> {
    const perms: Permissions[] = ['ohos.permission.MICROPHONE'];
    try {
      const result = await abilityAccessCtrl.createAtManager()
        .requestPermissionsFromUser(ctx, perms);
      return result.authResults.length > 0 && result.authResults[0] === 0;
    } catch (err) {
      const e = err as BusinessError;
      log('microphone permission request failed code=' + e.code + ' ' + e.message);
      return false;
    }
  }

  /** Open the capturer and start collecting. Returns false with the reason logged. */
  async start(): Promise<boolean> {
    if (this.capturer !== undefined) {
      return true;
    }
    this.chunks = [];
    this.bytes = 0;
    this.lastLevel = 0;
    this.loudest = 0;

    const streamInfo: audio.AudioStreamInfo = {
      samplingRate: this.rate === REFERENCE_SAMPLE_RATE
        ? audio.AudioSamplingRate.SAMPLE_RATE_24000
        : audio.AudioSamplingRate.SAMPLE_RATE_16000,
      channels: audio.AudioChannel.CHANNEL_1,
      sampleFormat: audio.AudioSampleFormat.SAMPLE_FORMAT_S16LE,
      encodingType: audio.AudioEncodingType.ENCODING_TYPE_RAW
    };
    const capturerInfo: audio.AudioCapturerInfo = {
      source: audio.SourceType.SOURCE_TYPE_MIC,
      capturerFlags: 0
    };

    try {
      const capturer = await audio.createAudioCapturer({
        streamInfo: streamInfo,
        capturerInfo: capturerInfo
      });
      capturer.on('readData', (buffer: ArrayBuffer) => {
        // Copy: this buffer is the audio stack's and gets reused.
        const chunk = new Uint8Array(buffer.byteLength);
        chunk.set(new Uint8Array(buffer));
        this.chunks.push(chunk);
        this.bytes += chunk.length;
        this.measure(chunk);
      });
      await capturer.start();
      this.capturer = capturer;
      log('capturing at ' + this.rate + ' Hz mono s16le');
      return true;
    } catch (err) {
      const e = err as BusinessError;
      log('could not start the capturer code=' + e.code + ' ' + e.message);
      this.capturer = undefined;
      return false;
    }
  }

  /**
   * Stop and hand back everything captured, as one ArrayBuffer of 16-bit
   * samples. Returns an empty buffer when nothing was recorded.
   */
  async stop(): Promise<ArrayBuffer> {
    const capturer = this.capturer;
    this.capturer = undefined;
    if (capturer !== undefined) {
      try {
        await capturer.stop();
        await capturer.release();
      } catch (err) {
        const e = err as BusinessError;
        log('stopping the capturer failed code=' + e.code + ' ' + e.message);
      }
    }

    const joined = new Uint8Array(this.bytes);
    let at = 0;
    for (const chunk of this.chunks) {
      joined.set(chunk, at);
      at += chunk.length;
    }
    this.chunks = [];
    log('captured ' + (this.bytes / 2 / this.rate).toFixed(1) + ' s');
    return joined.buffer as ArrayBuffer;
  }

  /** The block's level and the take's loudest sample. */
  private measure(chunk: Uint8Array): void {
    const samples: Int16Array = new Int16Array(chunk.buffer, chunk.byteOffset, Math.floor(chunk.length / 2));
    if (samples.length === 0) {
      return;
    }
    let sum: number = 0;
    for (let i = 0; i < samples.length; i++) {
      const s: number = samples[i] / 32768;
      sum += s * s;
      const a: number = Math.abs(s);
      if (a > this.loudest) {
        this.loudest = a;
      }
    }
    this.lastLevel = Math.sqrt(sum / samples.length);
  }

  /** Throw away a recording in progress without analysing it. */
  async discard(): Promise<void> {
    await this.stop();
    this.bytes = 0;
  }
}
