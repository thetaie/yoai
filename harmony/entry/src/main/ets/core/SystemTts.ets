/**
 * Speaking, through the platform rather than a model.
 *
 * design/voice.md settles why this and not the omni Talker: on this device the
 * Talker runs at 3.7x real time, so ten seconds of reply costs thirty-seven
 * seconds of the phone's whole attention, and streaming cannot rescue a ratio
 * above 1 -- it only moves the first word earlier and leaves the gaps. The
 * system engine is 1x by construction, costs no memory, and needs no model
 * file to reach the device.
 *
 * What is given up is real: the platform voice reads text, so it never sees the
 * hidden states that made an end-to-end Talker worth wanting, and the tone of
 * the reply cannot follow its content. That is a known trade and it is the
 * cheap side of it.
 *
 * The engine is created once and kept. Creation is the slow part and the second
 * utterance should not pay for it.
 */
import { textToSpeech } from '@kit.CoreSpeechKit';
import { BusinessError } from '@kit.BasicServicesKit';
import hilog from '@ohos.hilog';

function log(msg: string): void {
  hilog.info(0x0000, 'bodyclock', '%{public}s', msg);
}

/**
 * Where a spoken reply spent its time.
 *
 * Both clocks start when the reply begins, not when it is finished, because
 * generation and speech OVERLAP -- the first sentence is spoken while the
 * model writes the second. Timing them as if they were consecutive stages was
 * how an earlier version of this page reported a 5 s exchange as 13.9 s: it
 * added the whole generation to a `firstSound` that was already measured from
 * the same instant.
 */
export interface SpeakTiming {
  startedMs: number;    // reply start to the first sound. The wait that is felt.
  totalMs: number;      // reply start to the last synthesis finishing
  chars: number;
}

/** Sentence enders, Chinese and Western. Clause commas are deliberately absent:
 *  splitting a Chinese sentence at its commas is how a reply starts sounding
 *  like it is being spelled out. */
const ENDERS: string = '。！？!?…';

/**
 * One voice the platform offers, flattened out of textToSpeech.VoiceInfo.
 *
 * Flattened rather than passed through because `status` is optional in the
 * typings -- it only arrives from API 19 on -- and a page that has to test for
 * undefined on every row is a page that will forget to once.
 */
export interface Voice {
  person: number;
  language: string;
  style: string;
  gender: string;
  description: string;
  status: string;     // '' when this SDK level does not report one
}

export class SystemTts {
  private static engine: textToSpeech.TextToSpeechEngine | undefined = undefined;
  private static seq: number = 0;

  // Which voice speaks. Both are fixed at creation, so changing either rebuilds
  // the engine.
  //
  // person 0 is not a voice in the list -- it is the engine's "whatever this
  // device defaults to", and it is the only value that works without also
  // getting the language right. 'zh-CN' is the spelling the documentation uses;
  // listVoices() on this handset answers 'zh_CN'. Which spelling createEngine
  // wants alongside an explicit person is exactly what use() finds out, by
  // passing back whatever the engine itself reported.
  private static person: number = 0;
  private static language: string = 'zh-CN';

  // Why the last createEngine failed, for a page that has to say something
  // better than "it did not work". Cleared on every success.
  private static lastCode: number = 0;
  private static lastMessage: string = '';

  // Streaming state. The engine takes one utterance at a time, and there is no
  // documented queue parameter in the SDK typings -- `extraParams` is an
  // untyped bag and queueMode only appears in prose -- so the queue is kept
  // here and drained on each completion.
  private static pending: string[] = [];
  private static speaking: boolean = false;
  private static streamT0: number = 0;
  private static firstSoundMs: number = -1;
  private static ended: boolean = false;
  /** Set by stop(), cleared by beginStream(). Makes an interruption stick. */
  private static cancelled: boolean = false;
  private static onDrained: ((v: SpeakTiming | undefined) => void) | undefined = undefined;
  private static spokenChars: number = 0;
  /** A creation already in flight, so concurrent callers share one engine. */
  private static creating: Promise<boolean> | undefined = undefined;

  /** Whether an engine has already been created and is ready to speak. */
  static ready(): boolean {
    return SystemTts.engine !== undefined;
  }

  /**
   * Create the engine, or hand back the one already made.
   *
   * `online: 1` is the OFFLINE engine despite how it reads -- the field is
   * "which engine", not "is networked", and offline is the only one HarmonyOS
   * currently offers. Keeping it offline also keeps this lane consistent with
   * the rest of the app, where nothing said aloud has to leave the device.
   *
   * **Callers in flight share one creation.** Checking `engine` alone is not
   * enough: creation takes 0.4-0.6 s, and two calls inside that window both see
   * undefined and both create, after which the second assignment wins and the
   * first engine leaks with nothing holding it. That was unreachable while one
   * code path owned the engine, and stopped being unreachable when warming
   * became a second one -- toggle the chain on and hit Record immediately and
   * both fire. `creating` is cleared on settle so a failure can be retried, and
   * it is NOT called `pending` -- that name is already the queue of sentences
   * waiting to be spoken, three fields up.
   */
  static init(): Promise<boolean> {
    if (SystemTts.engine !== undefined) {
      return Promise.resolve(true);
    }
    if (SystemTts.creating !== undefined) {
      return SystemTts.creating;
    }
    SystemTts.creating = SystemTts.create();
    return SystemTts.creating;
  }

  private static create(): Promise<boolean> {
    return new Promise<boolean>((resolve: (v: boolean) => void) => {
      const params: textToSpeech.CreateEngineParams = {
        language: SystemTts.language,
        person: SystemTts.person,
        online: 1
      };
      textToSpeech.createEngine(params,
        (err: BusinessError, engine: textToSpeech.TextToSpeechEngine) => {
          if (err) {
            // The person and language are in the line because two different
            // failures (401 "voice not support" and 1002300005 from the
            // synthesizer) are indistinguishable without knowing what was asked
            // for -- which is how the first round of this ended up unreadable.
            SystemTts.lastCode = err.code;
            SystemTts.lastMessage = err.message;
            log('tts createEngine failed person=' + SystemTts.person +
                ' language=' + SystemTts.language +
                ' code=' + err.code + ' ' + err.message);
            SystemTts.creating = undefined;
            resolve(false);
            return;
          }
          SystemTts.lastCode = 0;
          SystemTts.lastMessage = '';
          SystemTts.engine = engine;
          log('tts engine ready person=' + SystemTts.person +
              ' language=' + SystemTts.language);
          SystemTts.creating = undefined;
          resolve(true);
        });
    });
  }

  //----------------------------------------------------------------------------
  // Which voices this device actually has
  //----------------------------------------------------------------------------

  /** The person the engine was built with. */
  static currentPerson(): number {
    return SystemTts.person;
  }

  /**
   * Ask the engine what it can speak with.
   *
   * There is no answering this from the documentation: the offline engine
   * speaks with whatever voice packs the device's system image happens to
   * carry, and that varies by model and by region. Asking the engine on the
   * device is the only way to know.
   *
   * The instance method is used rather than the module-level `listVoices()`
   * added in 5.1.1(19), because compatibleSdkVersion here is 5.0.0(12) and the
   * instance one has been there since 4.1.0(11). Same information.
   *
   * An empty array means no voices OR a failed query. The distinction is in the
   * log rather than the return, because there is nothing different a caller
   * could do about the two.
   */
  static voices(): Promise<Voice[]> {
    return new Promise<Voice[]>((resolve: (v: Voice[]) => void) => {
      SystemTts.init().then((ok: boolean) => {
        const engine: textToSpeech.TextToSpeechEngine | undefined = SystemTts.engine;
        if (!ok || engine === undefined) {
          resolve([]);
          return;
        }
        SystemTts.seq += 1;
        const query: textToSpeech.VoiceQuery = {
          requestId: 'bc-q' + SystemTts.seq,
          online: 1
        };
        engine.listVoices(query,
          (err: BusinessError, infos: Array<textToSpeech.VoiceInfo>) => {
            if (err) {
              log('tts listVoices failed code=' + err.code + ' ' + err.message);
              resolve([]);
              return;
            }
            const out: Voice[] = [];
            for (let i: number = 0; i < infos.length; i++) {
              const v: textToSpeech.VoiceInfo = infos[i];
              const one: Voice = {
                person: v.person,
                language: v.language,
                style: v.style,
                gender: v.gender,
                description: v.description,
                // Two "supported since 5.1.1(19), compatible is 5.0.0(12)"
                // warnings at compile time, and they are correct: on an older
                // device the field simply is not there. That is what the
                // undefined test is for, and why this is not a reason to raise
                // compatibleSdkVersion.
                status: v.status === undefined ? '' : v.status
              };
              out.push(one);
            }
            resolve(out);
          });
      });
    });
  }

  /** Why the last engine creation failed, or '' if none has. */
  static lastError(): string {
    return SystemTts.lastCode === 0
      ? ''
      : SystemTts.lastCode + ' ' + SystemTts.lastMessage;
  }

  /** 'zh_CN' <-> 'zh-CN'. Unchanged when there is no separator to swap. */
  private static otherSpelling(language: string): string {
    if (language.indexOf('_') >= 0) {
      return language.replace('_', '-');
    }
    if (language.indexOf('-') >= 0) {
      return language.replace('-', '_');
    }
    return language;
  }

  /**
   * Speak with one of the voices listVoices() reported. Resolves false if the
   * engine refuses it, in which case the previous voice is back.
   *
   * `person` lives in CreateEngineParams and nowhere else -- SpeakParams
   * carries only a requestId and an untyped bag -- so switching means shutting
   * the engine down and building another one. That cost is why this takes a
   * method call and not a field assignment: the caller should see it is an
   * operation, and should not do it between two sentences of one reply.
   *
   * The language comes from the voice rather than from the constant, and both
   * separator spellings are tried. This is not defensiveness: listVoices() on
   * this handset answers 'zh_CN' while every piece of documentation writes
   * 'zh-CN', createEngine takes an explicit person and a language together, and
   * nothing says which spelling it matches against. Two attempts settle it, and
   * the "engine ready" line in the log records the answer.
   */
  static use(voice: Voice): Promise<boolean> {
    return new Promise<boolean>((resolve: (v: boolean) => void) => {
      if (voice.person === SystemTts.person && SystemTts.engine !== undefined) {
        resolve(true);
        return;
      }
      const previousPerson: number = SystemTts.person;
      const previousLanguage: string = SystemTts.language;
      const spellings: string[] = [voice.language];
      const other: string = SystemTts.otherSpelling(voice.language);
      if (other !== voice.language) {
        spellings.push(other);
      }
      SystemTts.release();

      const attempt: (i: number) => void = (i: number) => {
        if (i >= spellings.length) {
          // Put back what was working. A refusal should cost a pause, not the
          // app's voice.
          SystemTts.person = previousPerson;
          SystemTts.language = previousLanguage;
          const code: number = SystemTts.lastCode;
          const message: string = SystemTts.lastMessage;
          SystemTts.init().then(() => {
            // Restored from the failure, so keep the failure's own error rather
            // than the success that undid it -- the caller is about to show it.
            SystemTts.lastCode = code;
            SystemTts.lastMessage = message;
            resolve(false);
          });
          return;
        }
        SystemTts.person = voice.person;
        SystemTts.language = spellings[i];
        SystemTts.init().then((ok: boolean) => {
          if (ok) {
            resolve(true);
            return;
          }
          attempt(i + 1);
        });
      };
      attempt(0);
    });
  }

  /**
   * Say one whole string. A convenience over the streaming path rather than a
   * second implementation of it: two ways to reach the engine means two places
   * that install a listener, and the one that is not driving the queue leaves
   * `speaking` wrong for the one that is.
   */
  static say(text: string): Promise<SpeakTiming | undefined> {
    SystemTts.beginStream();
    SystemTts.push(text);
    return SystemTts.endStream();
  }

  //----------------------------------------------------------------------------
  // Streaming: speak each sentence as the model finishes writing it
  //----------------------------------------------------------------------------

  /**
   * How much of `acc` is ready to speak, given that more is still coming.
   *
   * Returns the length of the prefix ending at the last sentence terminator, or
   * 0 when no sentence has closed yet. The caller keeps the remainder and asks
   * again on the next delta.
   *
   * Static and pure so the splitting rule is testable and lives in one place
   * rather than being re-improvised at each call site.
   */
  static completePrefix(acc: string): number {
    for (let i: number = acc.length - 1; i >= 0; i--) {
      if (ENDERS.indexOf(acc.charAt(i)) >= 0) {
        return i + 1;
      }
    }
    return 0;
  }

  /** Start a streamed reply. Resolves through endStream(). */
  static beginStream(): void {
    SystemTts.pending = [];
    SystemTts.speaking = false;
    SystemTts.streamT0 = Date.now();
    SystemTts.firstSoundMs = -1;
    SystemTts.ended = false;
    SystemTts.cancelled = false;
    SystemTts.spokenChars = 0;
    SystemTts.onDrained = undefined;
  }

  /** Hand over one finished sentence. Speaks at once if nothing else is. */
  static push(sentence: string): void {
    if (SystemTts.cancelled || sentence.trim().length === 0) {
      return;
    }
    SystemTts.spokenChars += sentence.length;
    SystemTts.pending.push(sentence);
    SystemTts.drain();
  }

  /**
   * No more sentences are coming. Resolves once the queue has been spoken.
   *
   * `totalMs` here is time to the last SYNTHESIS finishing, not to the last
   * sound leaving the speaker -- the engine reports completion when it has
   * produced the audio and plays it on its own. `startedMs` is the number that
   * matters anyway: it is how long the person sat in silence.
   */
  static endStream(): Promise<SpeakTiming | undefined> {
    return new Promise<SpeakTiming | undefined>(
      (resolve: (v: SpeakTiming | undefined) => void) => {
        SystemTts.ended = true;
        if (!SystemTts.speaking && SystemTts.pending.length === 0) {
          resolve(SystemTts.timing());
          return;
        }
        SystemTts.onDrained = resolve;
      });
  }

  private static timing(): SpeakTiming {
    return {
      startedMs: SystemTts.firstSoundMs < 0 ? 0 : SystemTts.firstSoundMs,
      totalMs: Date.now() - SystemTts.streamT0,
      chars: SystemTts.spokenChars
    };
  }

  private static drain(): void {
    const engine: textToSpeech.TextToSpeechEngine | undefined = SystemTts.engine;
    if (engine === undefined || SystemTts.speaking) {
      return;
    }
    if (SystemTts.pending.length === 0) {
      if (SystemTts.ended && SystemTts.onDrained !== undefined) {
        const done: (v: SpeakTiming | undefined) => void = SystemTts.onDrained;
        SystemTts.onDrained = undefined;
        done(SystemTts.timing());
      }
      return;
    }

    const next: string = SystemTts.pending.shift() as string;
    SystemTts.speaking = true;
    SystemTts.seq += 1;

    engine.setListener({
      onStart: (_rid: string, _resp: textToSpeech.StartResponse) => {
        if (SystemTts.firstSoundMs < 0) {
          SystemTts.firstSoundMs = Date.now() - SystemTts.streamT0;
        }
      },
      onComplete: (_rid: string, _resp: textToSpeech.CompleteResponse) => {
        SystemTts.speaking = false;
        SystemTts.drain();
      },
      onStop: (_rid: string, _resp: textToSpeech.StopResponse) => {
        SystemTts.speaking = false;
        SystemTts.pending = [];
        SystemTts.drain();
      },
      onError: (_rid: string, code: number, message: string) => {
        log('tts error code=' + code + ' ' + message);
        SystemTts.speaking = false;
        SystemTts.drain();
      }
    });

    const speakParams: textToSpeech.SpeakParams = { requestId: 'bc-' + SystemTts.seq };
    engine.speak(next, speakParams);
  }

  /**
   * Stop now, and stay stopped.
   *
   * Two things beyond telling the engine to be quiet, both of which cost a
   * hang or a zombie if left out:
   *
   * `cancelled` gates push(). The turn that was being spoken is still
   * generating -- an interruption does not reach the model -- and its next
   * sentence would otherwise start the queue up again a second after the
   * person interrupted it.
   *
   * A waiting endStream() is resolved here rather than left to `onStop`. That
   * callback only arrives if something was actually being spoken; interrupt
   * during the gap between two sentences and nothing fires, leaving whoever
   * awaited the drain waiting for the rest of the session.
   */
  static stop(): void {
    SystemTts.pending = [];
    SystemTts.cancelled = true;
    SystemTts.ended = true;
    if (SystemTts.engine !== undefined) {
      SystemTts.engine.stop();
    }
    SystemTts.speaking = false;
    if (SystemTts.onDrained !== undefined) {
      const done: (v: SpeakTiming | undefined) => void = SystemTts.onDrained;
      SystemTts.onDrained = undefined;
      done(SystemTts.timing());
    }
  }

  /** Let go of the engine. Speaking again after this pays for creation again. */
  static release(): void {
    if (SystemTts.engine !== undefined) {
      SystemTts.engine.shutdown();
      SystemTts.engine = undefined;
    }
    // A shut-down engine never calls onComplete, so a queue left mid-utterance
    // would keep `speaking` true and drain() would refuse to start anything
    // ever again. Anyone waiting is answered with what was spoken so far.
    SystemTts.pending = [];
    SystemTts.speaking = false;
    if (SystemTts.onDrained !== undefined) {
      const done: (v: SpeakTiming | undefined) => void = SystemTts.onDrained;
      SystemTts.onDrained = undefined;
      done(SystemTts.timing());
    }
  }
}
