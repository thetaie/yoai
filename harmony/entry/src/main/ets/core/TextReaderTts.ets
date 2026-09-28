/**
 * The platform's OTHER voice, behind the reading component rather than the TTS one.
 *
 * Why this exists: `@hms.ai.textToSpeech` hands out three voices on this
 * handset, only one installed, and every one of them has style
 * `interaction-broadcast` -- which is not selectable, because `style` appears in
 * those typings only as a field the engine reports. `online: 0` is refused
 * outright (`401 online type error`). So that API's ceiling is a broadcast
 * timbre, and it is not the voice the assistant speaks with.
 *
 * `@hms.ai.textReader` is a different surface with a different voice model:
 *
 *     textToSpeech   style is OUTPUT      person: number    online: 1 only
 *     textReader     style is INPUT       Person{tone,style}  online 0 or 1
 *
 * It is not a TTS primitive. It is Huawei's read-an-article product -- panel,
 * minibar, notification, previous and next, brand badge -- and using it means
 * accepting that UI. That was the trade-off; it was taken deliberately, because
 * a more natural voice is worth somebody else's panel.
 *
 * **It was a probe, and it has its answer: the same voice.** Driven with no
 * `person`, the reader speaks the sentence in the timbre `textToSpeech` already
 * gives -- the panel says 小艺朗读 and what comes out is the broadcast voice.
 * Two surfaces, two voice models on paper, one voice in the room.
 *
 * So the platform's ceiling for a third-party app IS the broadcast timbre. The
 * voices offered in the assistant's own settings are not reachable from either
 * API; they belong to the assistant. That makes design/voice.md's comparison
 * against omni a fair one rather than a configuration mistake, which is the
 * opposite of what this file was written to check.
 *
 * Kept rather than deleted because one thread is left and this is where it
 * would be pulled: `person` was never passed. `Person` is `{ tone: number,
 * style: string }` and the typings enumerate neither, but `interaction-broadcast`
 * is plainly one name in a scheme. Guessing the others is a poor bet and the
 * cost of being wrong is silent -- it would speak in the default and look like
 * a success.
 *
 * Offline by default: `online` is left unset, which the typings document as
 * "default to no". Anything this app says aloud stays on the device unless that
 * is decided separately.
 */
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import TextReader from '@hms.ai.textReader';
import hilog from '@ohos.hilog';

function log(msg: string): void {
  hilog.info(0x0000, 'yoai', '%{public}s', msg);
}

export class TextReaderTts {
  private static started: boolean = false;
  private static seq: number = 0;
  private static lastError: string = '';

  /** Whether init() has succeeded and the component is holding resources. */
  static ready(): boolean {
    return TextReaderTts.started;
  }

  /** Why the last call failed, for a page that has to say more than "no". */
  static error(): string {
    return TextReaderTts.lastError;
  }

  /**
   * Create the reader. Idempotent.
   *
   * `isVoiceBrandVisible` is required rather than optional, so it is a decision
   * and not a default: true, because this is a probe and seeing whose voice is
   * speaking is the point of it.
   */
  static async init(ctx: common.BaseContext): Promise<boolean> {
    if (TextReaderTts.started) {
      return true;
    }
    const param: TextReader.ReaderParam = {
      isVoiceBrandVisible: true,
      isMinibarNeeded: true,
      // Not set: `person`. The typings give Person as { tone: number, style:
      // string } and enumerate neither, so the first run takes whatever the
      // component defaults to -- which is the thing being listened to.
      keepBackgroundRunning: false
    };
    try {
      await TextReader.init(ctx, param);
      TextReaderTts.started = true;
      TextReaderTts.lastError = '';
      log('textReader init ok');
      return true;
    } catch (err) {
      const e = err as BusinessError;
      TextReaderTts.lastError = 'init ' + e.code + ' ' + e.message;
      log('textReader init failed code=' + e.code + ' ' + e.message);
      return false;
    }
  }

  /**
   * Read one passage aloud.
   *
   * `title` is mandatory and is spoken first, so it carries the shortest thing
   * that will not be mistaken for content. The text itself goes in `bodyInfo`.
   */
  static async say(ctx: common.BaseContext, text: string): Promise<boolean> {
    if (!await TextReaderTts.init(ctx)) {
      return false;
    }
    TextReaderTts.seq += 1;
    const info: TextReader.ReadInfo = {
      id: 'bc-' + TextReaderTts.seq,
      title: { text: ' ', isClickable: false },
      bodyInfo: text
    };
    try {
      await TextReader.start([info], info.id);
      log('textReader start ok, ' + text.length + ' chars');
      return true;
    } catch (err) {
      const e = err as BusinessError;
      TextReaderTts.lastError = 'start ' + e.code + ' ' + e.message;
      log('textReader start failed code=' + e.code + ' ' + e.message);
      return false;
    }
  }

  /** Stop and hand the resources back. Safe to call when nothing is running. */
  static async release(): Promise<void> {
    if (!TextReaderTts.started) {
      return;
    }
    try {
      await TextReader.stop();
    } catch (err) {
      const e = err as BusinessError;
      log('textReader stop failed code=' + e.code + ' ' + e.message);
    }
    try {
      await TextReader.release();
    } catch (err) {
      const e = err as BusinessError;
      log('textReader release failed code=' + e.code + ' ' + e.message);
    }
    TextReaderTts.started = false;
  }
}
