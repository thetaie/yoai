/**
 * Speaking text that already exists: a message's 朗读 and the settings test.
 *
 * A port of examples/desktop/src/renderer/src/tts.ts. Through the reading lane,
 * the only one this app speaks through: the text is one stream, fed in one go
 * and finished at once, and the core reads it exactly as it does a reply being
 * written.
 *
 * One stream at a time, because the engine reads one: a message's button and a
 * reply being read cannot both speak, and starting one stops the other.
 */
import { player } from './SpeechPlayer';
import { Reader } from './Reader';

/** AppStorage key: which message is being spoken, '' when none. Drives the buttons. */
export const SPEAKING_ID: string = 'speakingId';

export class Speak {
  private static speakingId: string = '';
  private static lastError: string = '';
  private static watchers: Set<() => void> = new Set<() => void>();

  private static announce(): void {
    AppStorage.setOrCreate(SPEAKING_ID, Speak.speakingId);
    Speak.watchers.forEach((w: () => void) => {
      w();
    });
  }

  /** '' when nothing is being spoken, else the id passed to speak(). */
  static speakingOf(): string {
    return Speak.speakingId;
  }

  /**
   * Speak `text`, tagged with `id` so a button can show its own state. Resolves
   * to '' when it started, or to a reason it did not. Asking again while
   * something is speaking stops the old one and starts this, which is what a
   * second tap on a different message means.
   *
   * The text as shown, markdown and emoji included: the core turns it into what
   * is spoken.
   */
  static async speak(id: string, text: string): Promise<string> {
    if (text.trim().length === 0) {
      return 'nothing to say';
    }
    if (!Reader.canSpeak()) {
      return 'the voice is not loaded';
    }
    await Speak.stop();

    // Set before the await: audio can start arriving the moment the stream
    // opens, and a button that has not yet flipped looks unresponsive.
    Speak.speakingId = id;
    Speak.lastError = '';
    Speak.announce();

    const stream: number = await Reader.begin();
    if (stream === 0) {
      Speak.speakingId = '';
      Speak.lastError = 'the voice could not start';
      Speak.announce();
      return Speak.lastError;
    }
    Reader.feed(stream, text);
    // Settled when the speaker is done, not when the engine is: finish()
    // resolves once the last sample is GENERATED, and the button offers 停止 for
    // as long as there is anything to stop.
    Reader.finish(stream)
      .then(() => Speak.quiet())
      .then(() => {
        Speak.settle(id, '');
      }, (e: Error) => {
        Speak.settle(id, e.message.length > 0 ? e.message : 'reading failed');
      });
    return '';
  }

  /**
   * Resolve once the player has nothing left to play -- also when it is
   * stopped, by this file or by a reply taking the speaker: either way this
   * utterance is no longer being heard.
   */
  private static quiet(): Promise<void> {
    return new Promise<void>((resolve: () => void) => {
      if (!player.active()) {
        resolve();
        return;
      }
      const off: () => void = player.watch(() => {
        if (!player.active()) {
          off();
          resolve();
        }
      });
    });
  }

  private static settle(id: string, error: string): void {
    if (Speak.speakingId !== id) {
      return;   // stopped, or replaced by another message
    }
    Speak.speakingId = '';
    Speak.lastError = error;
    Speak.announce();
  }

  /** Stop the engine and the playback. Safe to call when nothing is speaking. */
  static async stop(): Promise<void> {
    const was: string = Speak.speakingId;
    Speak.speakingId = '';
    if (was.length > 0) {
      Speak.announce();
    }
    await Reader.cancel();
  }

  /**
   * Resolve when the current utterance finishes, with its error or ''. For a
   * caller that wants to report a failure arriving after speak() returned.
   */
  static watchOnce(): Promise<string> {
    return new Promise<string>((resolve: (error: string) => void) => {
      const w: () => void = () => {
        if (Speak.speakingId.length === 0) {
          Speak.watchers.delete(w);
          resolve(Speak.lastError);
        }
      };
      Speak.watchers.add(w);
    });
  }
}
