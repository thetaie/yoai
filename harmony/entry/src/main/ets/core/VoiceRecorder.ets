/**
 * Microphone to 16-bit mono PCM at 16 kHz, which is exactly what the core takes.
 *
 * The capturer is opened at the rate the analysis needs rather than at whatever
 * the device prefers, so nothing resamples. That is deliberate and it is worth
 * knowing why: the core carries no resampler, and a resampler that differs from
 * the one a measurement was taken with changes the answer quietly. This project
 * has already spent an afternoon on a one-character transcription difference
 * that turned out to be exactly that.
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

const TAG: string = 'bodyclock-voice';

function log(msg: string): void {
  hilog.info(0x3200, TAG, '%{public}s', msg);
}

export const VOICE_SAMPLE_RATE: number = 16000;

/** Below this the within-recording comparison has nothing to compare. */
export const VOICE_MIN_SECONDS: number = 15;

export class VoiceRecorder {
  private capturer: audio.AudioCapturer | undefined = undefined;
  private chunks: Uint8Array[] = [];
  private bytes: number = 0;

  /** Whether the capturer is open and taking audio right now. */
  recording(): boolean {
    return this.capturer !== undefined;
  }

  /** Seconds captured so far, for a live read-out while recording. */
  seconds(): number {
    return this.bytes / 2 / VOICE_SAMPLE_RATE;
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

    const streamInfo: audio.AudioStreamInfo = {
      samplingRate: audio.AudioSamplingRate.SAMPLE_RATE_16000,
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
      });
      await capturer.start();
      this.capturer = capturer;
      log('capturing at 16 kHz mono s16le');
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
    log('captured ' + (this.bytes / 2 / VOICE_SAMPLE_RATE).toFixed(1) + ' s');
    return joined.buffer as ArrayBuffer;
  }

  /** Throw away a recording in progress without analysing it. */
  async discard(): Promise<void> {
    await this.stop();
    this.bytes = 0;
  }
}
