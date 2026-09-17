/**
 * The voice pipeline, with its working shown. Reached by typing /asr in the
 * composer -- it is a debug page, not a destination.
 *
 * Why it exists: everything design/voice.md argues for is a NUMBER, and numbers
 * measured on a workstation are not numbers measured on the device someone will
 * actually speak into. Microphone gain, room noise, model load time and encoder
 * speed on a phone are all unknowns that only a real recording settles.
 *
 * What it shows, and the order matters:
 *
 *   1. The prosody profile -- the part that is validated, needs no model, and is
 *      where the emotional reading comes from.
 *   2. The transcript and the model's own four tags.
 *
 * The model's `emotion` tag is shown LAST and labelled as not trusted. Its
 * vocabulary has no word for tired, depleted or calm, so the entire low-arousal
 * band collapses into NEUTRAL. It is here to be compared against the measured
 * arousal, not to be believed.
 *
 * Nothing on this page is a claim about the person. Every row is a deviation
 * from this recording's own middle, which is a thing the speaker can check.
 */
import { common } from '@kit.AbilityKit';
import { router } from '@kit.ArkUI';
import hilog from '@ohos.hilog';
import bodyclock from 'libbodyclock.so';
import { ICON_TOOLBAR, icon } from '../core/Icons';
import { LocalModelStore, PickedFile } from '../core/LocalModelStore';
import { DEFAULT_FAMILY, Palette, resolvePalette } from '../core/Palettes';
import { VOICE_MIN_SECONDS, VOICE_SAMPLE_RATE, VoiceRecorder } from '../core/VoiceRecorder';
import { VoiceModelRef, VoiceModelStore } from '../core/VoiceModelStore';
import { ModelStore } from '../core/ModelStore';
import { TextAxes, TextAxesReader, TextAxesResult } from '../core/TextAxes';
import { SpeakTiming, SystemTts, Voice } from '../core/SystemTts';
import { ChainPrompt, DEFAULT_CHAIN_PROMPT } from '../core/ChainPrompt';

//------------------------------------------------------------------------------
// The shape the core returns (src/bodyclock.h, bodyclock_voice_analyze)
//------------------------------------------------------------------------------

// Every field the core emits, because each z below is a raw measurement divided
// by a baseline, and a page that shows only the quotient cannot be checked.
interface UtteranceJson {
  start: number;
  duration: number;
  f0Mean: number;
  f0P10: number;
  f0P90: number;
  f0RangeSt: number;
  rmsMean: number;
  speechRatio: number;
  voicedFrames: number;
  zF0: number;
  zLoudness: number;
  zContinuity: number;
  arousal: number;
  text?: string;
}

// All six are the divisors behind the z-scores below, so all six are shown.
// A deviation you cannot check the arithmetic on is not working shown.
interface BaselineJson {
  f0Mean: number;
  f0Sd: number;
  rmsMean: number;
  rmsSd: number;
  speechMean: number;
  speechSd: number;
}

interface ProsodyJson {
  seconds: number;
  enough: boolean;
  baseline: BaselineJson;
  utterances: UtteranceJson[];
}

interface TranscriptJson {
  text: string;
  language: string;
  emotion: string;
  event: string;
  itn: boolean;
  filteredStrayCapitals: boolean;
  seconds: number;
}

interface AnalysisJson {
  audioSeconds: number;
  sampleRate: number;
  transcript?: TranscriptJson;
  prosody?: ProsodyJson;
}

@Entry
@Component
struct VoiceDebug {
  @StorageLink('paletteFamily') @Watch('resolveTheme') paletteFamily: string = DEFAULT_FAMILY;
  @StorageProp('isDark') @Watch('resolveTheme') isDark: boolean = false;
  @State private theme: Palette = resolvePalette(DEFAULT_FAMILY, false);

  @State private engine: boolean = false;
  @State private modelName: string = '';
  @State private modelLoading: boolean = false;

  @State private recording: boolean = false;
  @State private elapsed: number = 0;
  @State private analysing: boolean = false;
  @State private note: string = '';

  @State private audioSeconds: number = 0;
  @State private prosody: ProsodyJson | undefined = undefined;
  @State private transcript: TranscriptJson | undefined = undefined;

  // The text half. Separate state because it arrives seconds after the rest --
  // it needs a whole LLM turn, while prosody and recognition are already done.
  @State private axes: TextAxes | undefined = undefined;
  @State private axesReading: boolean = false;
  @State private axesNote: string = '';
  @State private axesProvider: string = '';
  @State private axesRaw: string = '';
  @State private axesSent: string = '';

  // Thread sweep. Present to settle one question: whether decode throughput is
  // flat in the thread count, which is what "one core already saturates the
  // memory controller" predicts, or rises through four to six cores, which
  // would make that wall per-core and not per-chip.
  @State private threads: number = 4;   // the core's own default
  @State private benching: boolean = false;
  @State private benchNote: string = '';

  // The whole chain: recognise, answer, speak. Here rather than on a page of
  // its own until the timings say whether it is worth a page.
  @State private chainOn: boolean = false;     // run it after the next recording
  @State private chainStage: string = '';
  @State private chainReply: string = '';
  @State private chainTiming: string = '';
  // Editable on the device: prompt work needs a loop measured in seconds, and
  // a rebuild per wording is not one.
  @State private chainPrompt: string = DEFAULT_CHAIN_PROMPT;
  @State private promptOpen: boolean = false;

  // Voices. Here because "can the voice be changed" has no answer off the
  // device: the offline engine speaks with whatever packs the system image
  // carries, so the list is a property of this phone and not of the SDK.
  @State private voices: Voice[] = [];
  @State private voicesAsked: boolean = false;
  @State private voicesBusy: boolean = false;
  @State private voicesNote: string = '';
  @State private person: number = 0;

  private recorder: VoiceRecorder = new VoiceRecorder();
  private ticker: number = -1;

  private resolveTheme(): void {
    this.theme = resolvePalette(this.paletteFamily, this.isDark);
  }

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  aboutToAppear(): void {
    this.resolveTheme();
    this.loadPrompt();
    this.engine = bodyclock.nativeVoiceAvailable();
    if (bodyclock.nativeVoiceLoaded()) {
      // Resident from an earlier visit. The name is worth recovering -- it is
      // the only way to tell which of several checkpoints is in memory.
      this.modelName = '(already loaded)';
      this.recoverName();
      return;
    }
    if (this.engine) {
      this.restoreModel();
    }
  }

  private async loadPrompt(): Promise<void> {
    this.chainPrompt = await ChainPrompt.get(this.ctx());
  }

  private async recoverName(): Promise<void> {
    const ref: VoiceModelRef | undefined = await VoiceModelStore.get(this.ctx());
    if (ref !== undefined) {
      this.modelName = ref.name;
    }
  }

  /**
   * Load the model picked on a previous visit, if there still is one.
   *
   * Not awaited by aboutToAppear -- the page must be usable while a 254 MB file
   * is being read, and prosody does not need the model at all. The status line
   * says what is happening and Record stays disabled only for as long as the
   * load runs.
   */
  private async restoreModel(): Promise<void> {
    const ref: VoiceModelRef | undefined = await VoiceModelStore.get(this.ctx());
    if (ref === undefined) {
      return;
    }
    const ok: boolean = await LocalModelStore.activateOne(ref.uri, ref.path);
    if (!ok) {
      // The file moved or was deleted. Forget it rather than failing the same
      // way on every launch, and say so once -- silently dropping to
      // prosody-only would look like the page had broken.
      await VoiceModelStore.clear(this.ctx());
      this.note = ref.name + ' is no longer where it was — pick it again.';
      return;
    }
    this.modelLoading = true;
    this.note = '';
    bodyclock.nativeVoiceLoad(ref.path, (type: string, content: string) => {
      this.modelLoading = false;
      if (type === 'ok') {
        this.modelName = ref.name;
      } else {
        this.note = content;
      }
    });
  }

  aboutToDisappear(): void {
    if (this.ticker !== -1) {
      clearInterval(this.ticker);
      this.ticker = -1;
    }
    // Leave the model resident: it is 254 MB off disk and the next visit to this
    // page almost certainly wants it again. Dropping the CAPTURER matters.
    if (this.recorder.recording()) {
      this.recorder.discard();
    }
  }

  //----------------------------------------------------------------------------
  // Actions
  //----------------------------------------------------------------------------

  private async pickModel(): Promise<void> {
    const picked: PickedFile | undefined = await LocalModelStore.pickGguf(this.ctx());
    if (picked === undefined) {
      return;
    }
    this.modelLoading = true;
    this.note = 'loading ' + picked.name + '…';
    bodyclock.nativeVoiceLoad(picked.path, (type: string, content: string) => {
      this.modelLoading = false;
      if (type === 'ok') {
        this.modelName = picked.name;
        this.note = '';
        // Remembered only after the load succeeded. A file that the engine
        // rejects is not worth reloading on every launch.
        VoiceModelStore.set(this.ctx(),
          { uri: picked.uri, path: picked.path, name: picked.name });
      } else {
        this.modelName = '';
        this.note = content;
      }
    });
  }

  private async toggleRecording(): Promise<void> {
    if (this.recording) {
      await this.finish();
      return;
    }

    // Barge-in. Someone reaching for the microphone while the reply is still
    // playing has stopped listening to it, and talking over your own device is
    // the single thing that makes a voice loop feel like a machine. Full-duplex
    // models are sold on this; here it is the queue already being drainable.
    //
    // Before the permission check on purpose: silence should follow the tap,
    // not the outcome of a dialog the person may take seconds over.
    SystemTts.stop();

    if (!await VoiceRecorder.requestPermission(this.ctx())) {
      this.note = 'microphone permission denied';
      return;
    }
    if (!await this.recorder.start()) {
      this.note = 'could not open the microphone';
      return;
    }
    this.prosody = undefined;
    this.transcript = undefined;
    this.note = '';
    this.recording = true;
    this.elapsed = 0;
    this.ticker = setInterval(() => { this.elapsed = this.recorder.seconds(); }, 200);
  }

  private async finish(): Promise<void> {
    if (this.ticker !== -1) {
      clearInterval(this.ticker);
      this.ticker = -1;
    }
    const pcm: ArrayBuffer = await this.recorder.stop();
    this.recording = false;
    this.elapsed = pcm.byteLength / 2 / VOICE_SAMPLE_RATE;

    if (pcm.byteLength === 0) {
      this.note = 'nothing was recorded';
      return;
    }

    this.analysing = true;
    bodyclock.nativeVoiceAnalyze(pcm, VOICE_SAMPLE_RATE, (type: string, content: string) => {
      this.analysing = false;
      if (type !== 'result') {
        this.note = content;
        return;
      }
      const parsed: AnalysisJson = JSON.parse(content) as AnalysisJson;
      this.audioSeconds = parsed.audioSeconds;
      this.prosody = parsed.prosody;
      this.transcript = parsed.transcript;
      this.note = '';
      if (parsed.transcript !== undefined && parsed.transcript.text !== '') {
        if (this.chainOn) {
          this.runChain(parsed.transcript.text, parsed.transcript.seconds);
        } else {
          this.readTextAxes(parsed.transcript.text);
        }
      }
    });
  }

  /**
   * Ask the configured chat model for the two text axes.
   *
   * Fired after the analysis lands rather than awaited inside it: prosody and
   * the transcript are ready in well under a second and should be on screen,
   * while this is a whole LLM turn -- seconds on a remote model, longer on a
   * local one.
   */
  private async readTextAxes(transcript: string): Promise<void> {
    this.axes = undefined;
    this.axesNote = '';
    this.axesRaw = '';
    this.axesSent = '';
    const provider: string = await ModelStore.activeId(this.ctx());
    if (provider.length === 0) {
      this.axesNote = 'no chat model configured — valence and dominance need one';
      return;
    }
    this.axesReading = true;
    const got: TextAxesResult = await TextAxesReader.read(provider, transcript);
    this.axesReading = false;
    this.axesProvider = got.provider;
    this.axesRaw = got.raw;
    this.axesSent = got.sent;
    if (got.axes === undefined) {
      this.axesNote = 'the model did not return a usable answer';
      return;
    }
    this.axes = got.axes;
  }

  /**
   * One timed generation against the configured on-device model.
   *
   * Deliberately NOT the text-axes call, which is the other local turn on this
   * page: a grammar-constrained verdict is about ten tokens, and ten tokens
   * divided by a wall clock is mostly noise. This asks for a long, boring,
   * predictable continuation instead, so the decode phase is long enough to
   * time.
   *
   * No grammar, because a grammar forces greedy decoding and this should run
   * with the same sampler the app uses -- the point is to measure the shipping
   * path, not a special one.
   *
   * Nothing is displayed from the reply. The measurement lands in the log, as
   * `on-device turn: ... prefill Nms decode Nms`, paired with the
   * `on-device threads:` line the setter writes.
   */
  private async benchDecode(): Promise<void> {
    const provider: string = await ModelStore.activeId(this.ctx());
    if (provider.length === 0) {
      this.benchNote = 'no chat model configured';
      return;
    }
    const rc: number = bodyclock.nativeLocalSetThreads(provider, this.threads);
    if (rc !== 0) {
      this.benchNote = 'the active model is not an on-device one — threads do not apply';
      return;
    }
    this.benching = true;
    this.benchNote = 'generating at ' + this.threads + ' threads…';

    const wire: string = JSON.stringify([{
      role: 'user',
      content: '请从一数到一百，只写数字，用顿号分隔，不要任何其它文字。'
    }]);
    let n: number = 0;
    bodyclock.nativeChat(provider, wire, '', '', (type: string, content: string) => {
      if (type === 'reply') {
        n += content.length;
      } else if (type === 'error') {
        this.benching = false;
        this.benchNote = content;
      } else if (type === 'end') {
        this.benching = false;
        this.benchNote = this.threads + ' threads · ' + n +
          ' chars generated — numbers are in the log';
      }
    });
  }

  /**
   * Ask the platform engine which voices this device has.
   *
   * One question only: how many. Huawei's offline pack is widely assumed to be
   * a single female voice, and an assumption about what a device contains is
   * worth exactly one button press to replace with a fact. The answer also goes
   * to the log, because it is a property of the handset that the next person
   * reading these timings will want and cannot re-derive.
   */
  private async probeVoices(): Promise<void> {
    this.voicesBusy = true;
    this.voicesNote = '';
    const got: Voice[] = await SystemTts.voices();
    this.voicesBusy = false;
    this.voicesAsked = true;
    this.voices = got;
    this.person = SystemTts.currentPerson();
    this.voicesNote = got.length === 0
      ? 'the engine returned nothing — an error code is in the log'
      : '';
    hilog.info(0x0000, 'bodyclock', '%{public}s', 'tts voices: ' + got.length);
    for (let i: number = 0; i < got.length; i++) {
      const v: Voice = got[i];
      hilog.info(0x0000, 'bodyclock', '%{public}s',
        '  person ' + v.person + ' ' + v.language + ' ' + v.gender +
        ' style=' + v.style + ' status=' + (v.status === '' ? '—' : v.status) +
        ' — ' + v.description);
    }
  }

  /**
   * Switch to one of them and say a sentence in it.
   *
   * Listing is not proof. A voice can appear in the list and still be a pack
   * that was never downloaded, in which case creating the engine with it fails
   * -- that is what makes this button the actual test and the list only a
   * prompt for it.
   */
  private async tryVoice(v: Voice): Promise<void> {
    this.voicesBusy = true;
    const ok: boolean = await SystemTts.use(v);
    this.person = SystemTts.currentPerson();
    this.voicesBusy = false;
    if (!ok) {
      // The engine's own code and message, because the two refusals seen so far
      // — 401 "voice not support" and 1002300005 from the synthesizer — mean
      // different things and a page that flattens them into "failed" makes the
      // next person go to the log anyway.
      this.voicesNote = 'person ' + v.person + ' would not start: ' +
        SystemTts.lastError() + ' — back on ' + this.person;
      return;
    }
    this.voicesNote = '';
    await SystemTts.say('今天过得怎么样？这是我现在说话的声音。');
  }

  /**
   * Recognise, answer, speak — the whole thing, timed at each handover.
   *
   * The point is not the demo. It is the four numbers: a chain is usable or not
   * depending on how long the person sits in silence after they stop talking,
   * and that total is made of parts with very different fixes. Recognition is
   * already known to run at 1.6x real time; the model's prefill scales with the
   * transcript; the platform voice is 1x by construction. Which one dominates
   * decides whether this becomes a page and what would have to change first.
   *
   * The reply is capped in the prompt rather than by `predict`, because a
   * length limit that truncates mid-sentence measures the wrong thing — what
   * matters is how long a reply someone would actually be read.
   */
  private async runChain(transcript: string, asrSeconds: number): Promise<void> {
    this.chainReply = '';
    this.chainTiming = '';
    const provider: string = await ModelStore.activeId(this.ctx());
    if (provider.length === 0) {
      this.chainStage = 'no chat model configured';
      return;
    }

    this.chainStage = 'starting the voice engine…';
    const t0: number = Date.now();
    const ok: boolean = await SystemTts.init();
    const ttsInitMs: number = Date.now() - t0;
    if (!ok) {
      this.chainStage = 'the system text-to-speech engine would not start';
      return;
    }

    this.chainStage = 'thinking…';
    const timing: SpeakTiming | undefined = await this.answerAndSpeak(provider, transcript);
    const reply: string = this.chainReply;
    if (reply === '') {
      this.chainStage = 'the model returned nothing';
      return;
    }
    this.chainStage = '';

    /*
     * Reported as two overlapping spans, not a sum of stages.
     *
     * Generation and speech run at the same time once the first sentence
     * closes, so adding them counts the overlap twice -- and the overlap is
     * most of the reply. `silence` is the only number a person experiences:
     * recognition, which must finish before the model can start, plus the wait
     * from there to the first sound. Everything after the first sound is heard
     * rather than waited through, however long generation keeps running.
     *
     * TTS engine creation is inside the first `silence` and nowhere after,
     * because the engine is kept; it is called out so a cold first exchange is
     * not mistaken for the steady state.
     */
    const firstSound: number = timing === undefined ? 0 : timing.startedMs / 1000;
    const silence: number = asrSeconds + ttsInitMs / 1000 + firstSound;

    this.chainTiming =
      'silence ' + this.fixed(silence, 2) + ' s' +
      '  (asr ' + this.fixed(asrSeconds, 2) +
      ' + ttsInit ' + this.fixed(ttsInitMs / 1000, 2) +
      ' + toFirstSound ' + this.fixed(firstSound, 2) + ')' +
      ' · ' + reply.length + ' chars' +
      (timing === undefined
        ? ' · speech failed'
        : ' · spoken over ' + this.fixed(timing.totalMs / 1000, 2) + ' s');

    hilog.info(0x0000, 'bodyclock', '%{public}s',
      'voice chain: audio ' + this.fixed(this.audioSeconds, 2) +
      's, asr ' + this.fixed(asrSeconds, 2) +
      's, ttsInit ' + this.fixed(ttsInitMs / 1000, 2) +
      's, toFirstSound ' + this.fixed(firstSound, 2) +
      's, SILENCE ' + this.fixed(silence, 2) +
      's, reply ' + reply.length + ' chars' +
      (timing === undefined
        ? ', speech FAILED'
        : ', spokenOver ' + this.fixed(timing.totalMs / 1000, 2) + 's'));
  }

  /**
   * One chat turn, spoken sentence by sentence as it is written.
   *
   * The reply is not waited for. Every delta is appended, and as soon as a
   * sentence closes it goes to the voice engine while the model is still
   * generating the next one. With the short replies this prompt asks for the
   * saving is small; it grows with the reply, and it is the difference between
   * a pause that scales with the answer's length and one that does not.
   *
   * The system prompt is as short as it can be while still doing its four jobs
   * -- length, register, no lists, no emoji. It is re-prefilled on EVERY turn,
   * because the engine frees its context when a turn ends, so every token in it
   * is paid for again each time. The persona sentence it used to open with was
   * 12 tokens of politeness on that bill.
   */
  private answerAndSpeak(provider: string, transcript: string): Promise<SpeakTiming | undefined> {
    return new Promise<SpeakTiming | undefined>(
      (resolve: (v: SpeakTiming | undefined) => void) => {
        const wire: string = JSON.stringify([{ role: 'user', content: transcript }]);
        let acc: string = '';     // everything so far
        let spoken: number = 0;   // how much of it has been handed to the engine

        this.chainReply = '';
        SystemTts.beginStream();

        const flush = (final: boolean): void => {
          const ready: number = final
            ? acc.length
            : spoken + SystemTts.completePrefix(acc.substring(spoken));
          if (ready > spoken) {
            SystemTts.push(acc.substring(spoken, ready));
            spoken = ready;
          }
        };

        bodyclock.nativeChat(provider, wire, this.chainPrompt, '',
          (type: string, content: string) => {
            if (type === 'reply') {
              acc += content;
              this.chainReply = acc;
              flush(false);
            } else if (type === 'error') {
              SystemTts.stop();
              resolve(undefined);
            } else if (type === 'end') {
              flush(true);
              SystemTts.endStream().then((t: SpeakTiming | undefined) => { resolve(t); });
            }
          });
      });
  }

  //----------------------------------------------------------------------------
  // Pieces
  //----------------------------------------------------------------------------

  /** Eight steps, the same shape the desktop harness prints. */
  private bar(arousal: number): string {
    const steps: string[] = ['▁', '▂', '▃', '▄', '▅', '▆', '▇', '█'];
    let i: number = Math.floor((arousal + 2) / 4 * 8);
    if (i < 0) { i = 0; }
    if (i > 7) { i = 7; }
    return steps[i];
  }

  private fixed(v: number, places: number): string {
    return v.toFixed(places);
  }

  /** A sign is always written, so a column of z-scores stays aligned and a
   *  positive deviation cannot be misread as a raw value. */
  private signed(v: number): string {
    return (v >= 0 ? '+' : '') + v.toFixed(2);
  }

  /** A deviation, or nothing at all when there is no distribution behind it.
   *  Withheld rather than weakened: see the note in prosodySection(). */
  private z(v: number): string {
    return this.pros().enough ? this.signed(v) : '';
  }

  /**
   * intensity -- distance from neutral, in ANY direction.
   *
   * The length of the three-deviation vector, not |arousal|. Arousal is a
   * weighted sum, so a high pitch pulling one way and a broken-up delivery
   * pulling the other can cancel to nearly zero on an utterance that is in fact
   * far from this speaker's middle. The norm cannot cancel.
   *
   * Derived here rather than in the core for the reason prosody.hpp gives about
   * arousal: today's combination of the three is a guess, and re-deriving it
   * must not need the audio back.
   */
  private intensity(u: UtteranceJson): number {
    return Math.sqrt(u.zF0 * u.zF0 +
                     u.zLoudness * u.zLoudness +
                     u.zContinuity * u.zContinuity);
  }

  /**
   * unpredictability -- how much the arousal series jumps, as the mean absolute
   * step between consecutive utterances.
   *
   * One number for the whole recording, so it belongs with the baseline and not
   * in a row. A steady reading and a see-sawing one can share an average and
   * are not the same delivery.
   *
   * Needs at least two steps to be a series rather than a single difference,
   * which is a stricter floor than the five the deviations need.
   */
  private unpredictability(): number {
    const us: UtteranceJson[] = this.pros().utterances;
    let sum: number = 0;
    for (let i: number = 1; i < us.length; i++) {
      sum += Math.abs(us[i].arousal - us[i - 1].arousal);
    }
    return us.length > 1 ? sum / (us.length - 1) : 0;
  }

  // A @Builder body may contain only UI component syntax -- no local
  // declarations -- so the two sections reach their data through these instead
  // of binding it to a name first. Both are only called from inside an
  // `if (... !== undefined)`.
  private pros(): ProsodyJson {
    return this.prosody as ProsodyJson;
  }

  private tran(): TranscriptJson {
    return this.transcript as TranscriptJson;
  }

  /**
   * One measurement per row, label left, value right, labels on a common width
   * so the values form a column. A debug readout is read by scanning down it,
   * and a middot-joined run of five numbers cannot be scanned.
   */
  @Builder
  metric(label: string, value: string) {
    Row() {
      Text(label)
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width(116)
      Text(value)
        .fontSize(11)
        .fontColor(this.theme.text_secondary)
        .layoutWeight(1)
    }
    .width('100%')
  }

  /**
   * One axis per row: what was measured, then how far that is from this
   * recording's own middle. Three columns so the raw values and the z-scores
   * each form a column that can be read straight down.
   *
   * `z` is a string rather than a number because two of these rows have no
   * deviation to report -- pitch range and voiced frames are context for the
   * axes above them, not axes themselves.
   */
  // `tail` is a deviation when `isZ`, otherwise context for the row above it.
  // Styled by KIND rather than by emptiness: a z-score earns the accent colour
  // because it is the number being read, and a note that borrowed that colour
  // would read as one more deviation.
  @Builder
  axis(label: string, value: string, tail: string, isZ: boolean) {
    Row() {
      Text(label)
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width(92)
      Text(value)
        .fontSize(11)
        .fontColor(this.theme.text_primary)
        .width(96)
      Text(tail)
        .fontSize(11)
        .fontWeight(isZ ? FontWeight.Medium : FontWeight.Normal)
        .fontColor(isZ ? this.theme.brand : this.theme.text_tertiary)
        .layoutWeight(1)
    }
    .width('100%')
  }

  @Builder
  header() {
    Row({ space: 8 }) {
      Button() {
        icon('chevron-left', ICON_TOOLBAR, this.theme.text_secondary)
      }
      .type(ButtonType.Circle)
      .backgroundColor(Color.Transparent)
      .onClick(() => router.back())

      Text('Voice')
        .fontSize(20)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.theme.text_primary)
        .layoutWeight(1)
      Text(this.engine ? 'engine on' : 'no engine')
        .fontSize(12)
        .fontColor(this.theme.text_tertiary)
    }
    .width('100%')
    .padding({ left: 16, right: 16, top: 12, bottom: 12 })
  }

  @Builder
  controls() {
    Column({ space: 10 }) {
      Row({ space: 10 }) {
        Button(this.recording ? 'Stop' : 'Record')
          .layoutWeight(1)
          .height(44)
          .backgroundColor(this.recording ? this.theme.danger : this.theme.brand)
          .fontColor(this.theme.on_brand)
          .enabled(!this.analysing && !this.modelLoading)
          .onClick(() => { this.toggleRecording(); })

        Button(this.modelName === '' ? 'Pick model' : 'Change')
          .height(44)
          .backgroundColor(this.theme.bg_surface)
          .fontColor(this.theme.text_primary)
          .enabled(this.engine && !this.recording && !this.analysing)
          .onClick(() => { this.pickModel(); })
      }
      .width('100%')

      Text(this.statusLine())
        .fontSize(12)
        .fontColor(this.theme.text_tertiary)
        .width('100%')

      if (this.note !== '') {
        Text(this.note)
          .fontSize(12)
          .fontColor(this.theme.danger)
          .width('100%')
      }
    }
    .width('100%')
    .padding({ left: 16, right: 16, bottom: 12 })
  }

  private statusLine(): string {
    if (this.modelLoading) { return 'loading the model…'; }
    if (this.recording)    { return 'recording ' + this.fixed(this.elapsed, 1) + ' s'; }
    if (this.analysing)    { return 'analysing…'; }
    const model: string = this.modelName === ''
      ? 'no recognition model — prosody only'
      : 'model: ' + this.modelName;
    return model + ' · 16 kHz mono';
  }

  @Builder
  chainSection() {
    Column({ space: 8 }) {
      Row() {
        Text('Recognise, answer, speak')
          .fontSize(15)
          .fontWeight(FontWeight.Medium)
          .fontColor(this.theme.text_primary)
          .layoutWeight(1)
        Toggle({ type: ToggleType.Switch, isOn: this.chainOn })
          .enabled(!this.recording && !this.analysing)
          .onChange((on: boolean) => { this.chainOn = on; })
      }
      .width('100%')

      Text(this.chainOn
        ? 'The next recording goes through the whole chain instead of the axes.'
        : 'Off — a recording is measured but not answered.')
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')

      Row({ space: 8 }) {
        Button(this.promptOpen ? 'Hide prompt' : 'Edit prompt')
          .height(32)
          .layoutWeight(1)
          .backgroundColor(this.theme.bg_surface)
          .fontColor(this.theme.text_primary)
          .fontSize(12)
          .onClick(() => { this.promptOpen = !this.promptOpen; })
        if (this.promptOpen) {
          Button('Default')
            .height(32)
            .backgroundColor(this.theme.bg_surface)
            .fontColor(this.theme.text_secondary)
            .fontSize(12)
            .onClick(() => {
              this.chainPrompt = DEFAULT_CHAIN_PROMPT;
              ChainPrompt.set(this.ctx(), DEFAULT_CHAIN_PROMPT);
            })
        }
      }
      .width('100%')

      if (this.promptOpen) {
        TextArea({ text: this.chainPrompt })
          .width('100%')
          .height(220)
          .fontSize(12)
          .backgroundColor(this.theme.bg_surface)
          .fontColor(this.theme.text_primary)
          // Saved per keystroke rather than behind a button: there is no
          // partially-valid prompt, and a save step is one more thing to
          // forget between a wording and the recording that tests it.
          .onChange((v: string) => {
            this.chainPrompt = v;
            ChainPrompt.set(this.ctx(), v);
          })

        Text('Saved as you type. Cached after the first reply of a session, ' +
             'so length costs almost nothing — examples beat rules.')
          .fontSize(11)
          .fontColor(this.theme.text_tertiary)
          .width('100%')
      }

      if (this.chainStage !== '') {
        Text(this.chainStage)
          .fontSize(12)
          .fontColor(this.theme.text_secondary)
          .width('100%')
      }
      if (this.chainReply !== '') {
        Text(this.chainReply)
          .fontSize(13)
          .fontColor(this.theme.text_primary)
          .width('100%')
      }
      if (this.chainTiming !== '') {
        Text(this.chainTiming)
          .fontSize(11)
          .fontColor(this.theme.brand)
          .width('100%')

        Button('Say it again')
          .height(34)
          .backgroundColor(this.theme.bg_surface)
          .fontColor(this.theme.text_primary)
          .fontSize(13)
          .onClick(() => { SystemTts.say(this.chainReply); })
      }
    }
    .width('100%')
    .padding(16)
  }

  @Builder
  voicesSection() {
    Column({ space: 8 }) {
      Text('Voices')
        .fontSize(15)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.theme.text_primary)
        .width('100%')

      Button(this.voicesBusy ? 'asking…' : 'List voices')
        .width('100%')
        .height(40)
        .backgroundColor(this.theme.bg_surface)
        .fontColor(this.theme.text_primary)
        .enabled(!this.voicesBusy && !this.recording && !this.analysing)
        .onClick(() => { this.probeVoices(); })

      if (this.voicesAsked && this.voices.length > 0) {
        ForEach(this.voices, (v: Voice) => {
          Column({ space: 3 }) {
            Row({ space: 6 }) {
              Text('person ' + v.person)
                .fontSize(12)
                .fontWeight(FontWeight.Medium)
                .fontColor(this.person === v.person ? this.theme.brand : this.theme.text_primary)
                .layoutWeight(1)
              Button('Hear it')
                .height(28)
                .padding({ left: 12, right: 12 })
                .backgroundColor(this.theme.bg_surface)
                .fontColor(this.theme.text_primary)
                .fontSize(12)
                .enabled(!this.voicesBusy)
                .onClick(() => { this.tryVoice(v); })
            }
            .width('100%')

            this.metric('language', v.language === '' ? '—' : v.language)
            this.metric('gender', v.gender === '' ? '—' : v.gender)
            this.metric('style', v.style === '' ? '—' : v.style)
            // Only filled from API 19 on. Where it is blank, whether the pack is
            // on the device is not knowable from here -- Hear it settles it.
            this.metric('status', v.status === '' ? '(not reported)' : v.status)
            if (v.description !== '') {
              Text(v.description)
                .fontSize(11)
                .fontColor(this.theme.text_secondary)
                .width('100%')
            }
          }
          .width('100%')
          .padding({ top: 8, bottom: 8 })
          .border({ width: { bottom: 1 }, color: this.theme.divider })
        }, (v: Voice) => v.person + ':' + v.style)
      }

      if (this.voicesNote !== '') {
        Text(this.voicesNote)
          .fontSize(12)
          .fontColor(this.theme.danger)
          .width('100%')
      }

      Text(this.voicesAsked && this.voices.length > 0
        ? 'Speaking with ' + this.person + '. The person is fixed when the ' +
          'engine is created, so each switch shuts it down and builds another ' +
          '— fine here, not something to do between two sentences of a reply.'
        : 'The offline engine speaks with whatever voice packs this handset ' +
          'ships; the list is a property of the device, not of the SDK.')
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')
    }
    .width('100%')
    .padding(16)
  }

  @Builder
  benchSection() {
    Column({ space: 8 }) {
      Text('Threads')
        .fontSize(15)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.theme.text_primary)
        .width('100%')

      Row({ space: 6 }) {
        ForEach([1, 2, 4, 6, 8], (n: number) => {
          Button(n.toString())
            .height(34)
            .layoutWeight(1)
            .backgroundColor(this.threads === n ? this.theme.brand : this.theme.bg_surface)
            .fontColor(this.threads === n ? this.theme.on_brand : this.theme.text_primary)
            .fontSize(13)
            .enabled(!this.benching)
            .onClick(() => { this.threads = n; })
        }, (n: number) => n.toString())
      }
      .width('100%')

      Button(this.benching ? 'generating…' : 'Bench decode')
        .width('100%')
        .height(40)
        .backgroundColor(this.theme.bg_surface)
        .fontColor(this.theme.text_primary)
        .enabled(!this.benching && !this.recording && !this.analysing)
        .onClick(() => { this.benchDecode(); })

      Text(this.benchNote === ''
        ? 'Counts to a hundred on the on-device model and leaves the timings ' +
          'in the log — prefill scales with cores, decode should not if ' +
          'bandwidth is the wall.'
        : this.benchNote)
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')
    }
    .width('100%')
    .padding(16)
  }

  @Builder
  prosodySection() {
    Column({ space: 8 }) {
      Text(this.pros().enough
        ? 'Within the recording, against itself'
        : 'Measured, but too short to normalise')
        .fontSize(15)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.theme.text_primary)
        .width('100%')

      Column({ space: 3 }) {
        // A baseline is only a baseline once there is a distribution. Below the
        // floor the mean is the average of two numbers and the sd is the 1.0
        // fallback prosody.cpp substitutes when the spread is zero, so printing
        // either would invent a reference point that does not exist.
        if (this.pros().enough) {
          this.metric('F0 mean', this.fixed(this.pros().baseline.f0Mean, 1) + ' Hz')
          this.metric('F0 sd', this.fixed(this.pros().baseline.f0Sd, 1) + ' Hz')
          this.metric('RMS mean', this.fixed(this.pros().baseline.rmsMean, 4))
          this.metric('RMS sd', this.fixed(this.pros().baseline.rmsSd, 4))
          this.metric('speech mean', this.fixed(this.pros().baseline.speechMean, 3))
          this.metric('speech sd', this.fixed(this.pros().baseline.speechSd, 3))
          this.metric('unpredictability',
            this.fixed(this.unpredictability(), 2) + '  mean |Δ arousal|')
        }
        this.metric('utterances', this.pros().utterances.length.toString())
        this.metric('audio', this.fixed(this.audioSeconds, 1) + ' s')
        this.metric('computed in', this.fixed(this.pros().seconds, 2) + ' s')
      }
      .width('100%')

      if (!this.pros().enough) {
        // NOT a refusal, which is what this used to be. Most real notes run 5-15
        // seconds, so the old message was what the page showed almost every time.
        //
        // The raw measurements are well defined for a single utterance and are
        // what a debug page is for; it is only the DEVIATIONS that need a
        // distribution. Those are withheld rather than weakened, because below
        // the floor they are not a noisy signal but a constant: prosody.cpp
        // divides by the population sd, so at n=2 every z-score is exactly +1.00
        // and -1.00 no matter what was said, and at n=1 all three are 0.00.
        Text('Deviations and arousal need 5 utterances (roughly ' +
             VOICE_MIN_SECONDS + ' s of speech); this has ' +
             this.pros().utterances.length + '. With no distribution to compare ' +
             'against, a z-score here would be the same number for every ' +
             'recording ever made, so only what was measured is shown.')
          .fontSize(12)
          .fontColor(this.theme.text_secondary)
          .width('100%')
      }

      ForEach(this.pros().utterances, (u: UtteranceJson, index: number) => {
          Column({ space: 3 }) {
            // The headline: which utterance, when, and the one number the three
            // axes below are combined into.
            Row({ space: 6 }) {
              Text('#' + index)
                .fontSize(12)
                .fontWeight(FontWeight.Medium)
                .fontColor(this.theme.text_primary)
                .width(30)
              Text(this.fixed(u.start, 1) + '–' + this.fixed(u.start + u.duration, 1) + ' s')
                .fontSize(11)
                .fontColor(this.theme.text_tertiary)
                .layoutWeight(1)
              if (this.pros().enough) {
                Text(this.bar(u.arousal))
                  .fontSize(13)
                  .fontColor(this.theme.brand)
                Text('arousal ' + this.signed(u.arousal))
                  .fontSize(12)
                  .fontWeight(FontWeight.Medium)
                  .fontColor(this.theme.text_primary)
              }
            }
            .width('100%')
            .padding({ bottom: 2 })

            this.axis('pitch', this.fixed(u.f0Mean, 1) + ' Hz', this.z(u.zF0), true)
            this.axis('  range', this.fixed(u.f0RangeSt, 1) + ' st',
              'p10 ' + this.fixed(u.f0P10, 0) + ' · p90 ' + this.fixed(u.f0P90, 0) + ' Hz',
              false)
            this.axis('loudness', this.fixed(u.rmsMean, 4), this.z(u.zLoudness), true)
            this.axis('continuity', this.fixed(u.speechRatio, 3), this.z(u.zContinuity), true)
            this.axis('  voiced', u.voicedFrames + ' frames', '', false)

            if (this.pros().enough) {
              this.axis('intensity', this.fixed(this.intensity(u), 2),
                'distance from neutral, any direction', false)
            }

            if (u.text !== undefined && u.text !== '') {
              Text(u.text)
                .fontSize(12)
                .fontColor(this.theme.text_secondary)
                .width('100%')
                .padding({ top: 3 })
            }
          }
          .width('100%')
          .padding({ top: 8, bottom: 8 })
          .border({ width: { bottom: 1 }, color: this.theme.divider })
        }, (u: UtteranceJson, index: number) => index + ':' + u.start)
    }
    .width('100%')
    .padding(16)
  }

  @Builder
  textAxesSection() {
    Column({ space: 8 }) {
      Text('From the words')
        .fontSize(15)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.theme.text_primary)
        .width('100%')

      if (this.axesReading) {
        Text('asking the chat model…')
          .fontSize(12)
          .fontColor(this.theme.text_tertiary)
          .width('100%')
      } else if (this.axesNote !== '') {
        Text(this.axesNote)
          .fontSize(12)
          .fontColor(this.theme.text_tertiary)
          .width('100%')
      } else if (this.axes !== undefined) {
        Column({ space: 3 }) {
          this.axis('valence', this.signed((this.axes as TextAxes).valence),
            'unpleasant → pleasant', false)
          this.axis('dominance', this.signed((this.axes as TextAxes).dominance),
            'acted upon → in control', false)
        }
        .width('100%')

        // The whole reason both channels are measured. Valence is easy to mask
        // in words and arousal is hard to mask in voice, so the two disagreeing
        // is information, not an error -- cheerful words in a tired voice is a
        // thing only two channels can show.
        Text('Read against the arousal above. The two are not competing ' +
             'measurements of one thing; they answer different questions, and ' +
             'where they disagree is the part worth looking at.')
          .fontSize(11)
          .fontColor(this.theme.text_tertiary)
          .width('100%')
      }

      // The three inputs behind the verdict. A number that looks wrong is only
      // debuggable next to WHICH model answered and WHAT it was asked about:
      // a misjudgment, an unexpected provider, and a transcript that is not
      // what was said all produce the same surprising number.
      if (this.axesProvider !== '') {
        Column({ space: 3 }) {
          this.metric('answered by', this.axesProvider)
          this.metric('sent', this.axesSent)
          this.metric('raw reply', this.axesRaw === '' ? '(nothing)' : this.axesRaw)
        }
        .width('100%')
        .padding({ top: 4 })
      }
    }
    .width('100%')
    .padding(16)
  }

  @Builder
  transcriptSection() {
    Column({ space: 8 }) {
      Text('What was said')
        .fontSize(15)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.theme.text_primary)
        .width('100%')

      Text(this.tran().text === '' ? '(nothing)' : this.tran().text)
        .fontSize(13)
        .fontColor(this.theme.text_primary)
        .width('100%')

      Column({ space: 3 }) {
        this.metric('language', this.tran().language === '' ? '—' : this.tran().language)
        this.metric('event', this.tran().event === '' ? '—' : this.tran().event)
        this.metric('ITN', this.tran().itn ? 'on' : 'off')
        this.metric('transcribed in', this.fixed(this.tran().seconds, 2) + ' s')
        this.metric('speed',
          this.fixed(this.audioSeconds / Math.max(this.tran().seconds, 0.001), 1) + '× real time')
      }
      .width('100%')

      if (this.tran().filteredStrayCapitals) {
        // A known failure of this model on some audio, at every precision. It
        // matters because a wrong transcription does not look like silence, it
        // looks like text.
        Text('A run of isolated capital letters was filtered out of the transcript.')
          .fontSize(11)
          .fontColor(this.theme.danger)
          .width('100%')
      }

      Text('model says: ' + (this.tran().emotion === '' ? '—' : this.tran().emotion) +
           ' — not trusted. Its labels are all high-arousal except SAD, so ' +
           'tired, depleted and calm all come back NEUTRAL. Compare it against ' +
           'the measured arousal above; disagreement is the only interesting part.')
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')
    }
    .width('100%')
    .padding(16)
  }

  build() {
    Column() {
      this.header()
      this.controls()

      Scroll() {
        Column() {
          this.chainSection()
          this.voicesSection()
          this.benchSection()
          if (this.prosody !== undefined) {
            this.prosodySection()
          }
          if (this.transcript !== undefined) {
            this.transcriptSection()
          }
          if (this.axes !== undefined || this.axesReading || this.axesNote !== '') {
            this.textAxesSection()
          }
          if (this.prosody === undefined && this.transcript === undefined && !this.analysing) {
            Text('Record something and the measurements land here. ' +
                 'Without a recognition model you still get the prosody half, ' +
                 'which is where the emotional reading comes from.')
              .fontSize(12)
              .fontColor(this.theme.text_tertiary)
              .width('100%')
              .padding(16)
          }
        }
        .width('100%')
      }
      .layoutWeight(1)
      .align(Alignment.Top)
    }
    .width('100%')
    .height('100%')
    .backgroundColor(this.theme.bg_page)
  }
}
