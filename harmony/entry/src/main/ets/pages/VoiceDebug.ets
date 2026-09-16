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
import bodyclock from 'libbodyclock.so';
import { ICON_TOOLBAR, icon } from '../core/Icons';
import { LocalModelStore, PickedFile } from '../core/LocalModelStore';
import { DEFAULT_FAMILY, Palette, resolvePalette } from '../core/Palettes';
import { VOICE_MIN_SECONDS, VOICE_SAMPLE_RATE, VoiceRecorder } from '../core/VoiceRecorder';

//------------------------------------------------------------------------------
// The shape the core returns (src/bodyclock.h, bodyclock_voice_analyze)
//------------------------------------------------------------------------------

interface UtteranceJson {
  start: number;
  duration: number;
  f0Mean: number;
  f0RangeSt: number;
  speechRatio: number;
  zF0: number;
  zLoudness: number;
  zContinuity: number;
  arousal: number;
  text?: string;
}

interface BaselineJson {
  f0Mean: number;
  f0Sd: number;
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
    this.engine = bodyclock.nativeVoiceAvailable();
    if (bodyclock.nativeVoiceLoaded()) {
      this.modelName = '(already loaded)';
    }
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
  prosodySection() {
    Column({ space: 8 }) {
      Text('Within the recording, against itself')
        .fontSize(15)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.theme.text_primary)
        .width('100%')

      Text('baseline F0 ' + this.fixed(this.pros().baseline.f0Mean, 1) +
           ' Hz (sd ' + this.fixed(this.pros().baseline.f0Sd, 1) + ') · ' +
           this.pros().utterances.length + ' utterances over ' +
           this.fixed(this.audioSeconds, 1) + ' s · ' +
           this.fixed(this.pros().seconds, 2) + ' s to compute')
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')

      if (!this.pros().enough) {
        // The honest output below five utterances is nothing. A z-score over
        // three units is noise, and a weak number shown confidently is the exact
        // failure this whole design is trying to avoid.
        Text('Too few utterances to normalise against. Record at least ' +
             VOICE_MIN_SECONDS + ' seconds of continuous speech — ' +
             'until then there is nothing to compare against, and a number here ' +
             'would be noise wearing a decimal point.')
          .fontSize(12)
          .fontColor(this.theme.text_secondary)
          .width('100%')
      } else {
        ForEach(this.pros().utterances, (u: UtteranceJson, index: number) => {
          Column({ space: 2 }) {
            Row({ space: 6 }) {
              Text('#' + index)
                .fontSize(11)
                .fontColor(this.theme.text_tertiary)
                .width(26)
              Text(this.fixed(u.start, 1) + 's')
                .fontSize(11)
                .fontColor(this.theme.text_tertiary)
                .width(44)
              Text(this.bar(u.arousal))
                .fontSize(13)
                .fontColor(this.theme.brand)
              Text(this.fixed(u.arousal, 2))
                .fontSize(13)
                .fontWeight(FontWeight.Medium)
                .fontColor(this.theme.text_primary)
                .width(48)
              Text('F0 ' + this.fixed(u.f0Mean, 0) + ' Hz')
                .fontSize(11)
                .fontColor(this.theme.text_tertiary)
                .layoutWeight(1)
            }
            .width('100%')

            Text('z: pitch ' + this.fixed(u.zF0, 2) +
                 ' · loudness ' + this.fixed(u.zLoudness, 2) +
                 ' · continuity ' + this.fixed(u.zContinuity, 2) +
                 ' · range ' + this.fixed(u.f0RangeSt, 1) + ' st')
              .fontSize(10)
              .fontColor(this.theme.text_tertiary)
              .width('100%')

            if (u.text !== undefined && u.text !== '') {
              Text(u.text)
                .fontSize(12)
                .fontColor(this.theme.text_secondary)
                .width('100%')
            }
          }
          .width('100%')
          .padding({ top: 6, bottom: 6 })
          .border({ width: { bottom: 1 }, color: this.theme.divider })
        }, (u: UtteranceJson, index: number) => index + ':' + u.start)
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

      Text('language ' + (this.tran().language === '' ? '—' : this.tran().language) +
           ' · event ' + (this.tran().event === '' ? '—' : this.tran().event) +
           ' · itn ' + (this.tran().itn ? 'on' : 'off') +
           ' · ' + this.fixed(this.tran().seconds, 2) + ' s' +
           ' (' + this.fixed(this.audioSeconds / Math.max(this.tran().seconds, 0.001), 0) + '× real time)')
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
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
          if (this.prosody !== undefined) {
            this.prosodySection()
          }
          if (this.transcript !== undefined) {
            this.transcriptSection()
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
