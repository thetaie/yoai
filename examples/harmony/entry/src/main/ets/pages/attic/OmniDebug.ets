/**
 * The omni lane, and the one number that decides it. Reached by typing /omni.
 *
 * This page is NOT a second version of /asr. They test different things, and
 * keeping them apart is the point:
 *
 *   /asr   SenseVoice plus the prosody profile -- what the app will actually
 *          ship. Transcript, clause boundaries, arousal per utterance.
 *   /omni  MNN plus Qwen2.5-Omni-3B, for the single capability ggml cannot
 *          provide: the model answering in its own voice.
 *
 * What this page is for is a measurement, not a feature. Generating speech ran
 * about 3x slower than real time on a desktop CPU, and a desktop figure does not
 * transfer to a phone. `speechRealtime` below is seconds of computation per
 * second of speech, for the **Talker alone**: above 1 the model cannot talk in
 * real time. Measured here at 3.70.
 *
 * The line under it asks whether playing the reply as it arrives would rescue
 * that, and the answer measured here is no. Nine blocks carried 10.34 s of
 * speech over 38.5 s — one every 4.3 s holding 1.15 s of audio, so playback
 * drains each in 1.15 s and then waits 3.2 s: **27% speech, 73% silence**,
 * which is the ratio itself. Streaming moves the first word much earlier
 * (14.7 s against 47.7 s) and cannot close the gaps. The numbers stay because
 * that is what the conclusion rests on.
 *
 * Two things this page deliberately does not do. It does not show the model's
 * opinion of the speaker's emotion as a finding -- the 3B was measured not to
 * hear tone at all (same words at three tape speeds, same answer), so its
 * emotional read comes from the words, which the text axes already cover. And it
 * does not transcribe: SenseVoice is a fraction of the size and is the only
 * thing here that yields the 60 ms alignment the clause cut runs on.
 */
import { common } from '@kit.AbilityKit';
import { audio } from '@kit.AudioKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { picker } from '@kit.CoreFileKit';
import { router } from '@kit.ArkUI';
import fileUri from '@ohos.file.fileuri';
import fs from '@ohos.file.fs';
import yoai from 'libyoai.so';
import { ICON_TOOLBAR, icon } from '../core/Icons';
import { DEFAULT_FAMILY, Palette, resolvePalette } from '../core/Palettes';
import { VOICE_SAMPLE_RATE, VoiceRecorder } from '../core/VoiceRecorder';

/** What yoai_omni_ask returns (src/yoai.h). */
interface OmniJson {
  text: string;
  audioSeconds: number;
  seconds: number;
  thinkerSeconds: number;
  speechSeconds: number;
  speechElapsed: number;
  speechRealtime: number;
  firstAudioSeconds: number;
  audioBlocks: number;
}

/** The Talker's output rate, which is NOT the 16 kHz of the input side. */
const SPEECH_SAMPLE_RATE: number = 24000;

/** What the engine survives. Past roughly 30 s MNN segfaults in its front end. */
const OMNI_MAX_SECONDS: number = 28;

@Entry
@Component
struct OmniDebug {
  @StorageLink('paletteFamily') @Watch('resolveTheme') paletteFamily: string = DEFAULT_FAMILY;
  @StorageProp('isDark') @Watch('resolveTheme') isDark: boolean = false;
  @State private theme: Palette = resolvePalette(DEFAULT_FAMILY, false);

  @State private engine: boolean = false;
  @State private modelName: string = '';
  @State private busy: string = '';
  @State private note: string = '';

  @State private recording: boolean = false;
  @State private elapsed: number = 0;
  @State private speak: boolean = true;
  /** The model's own config.json asks for 5, which is four DiT forwards. */
  @State private ditSteps: number = 5;

  @State private reply: OmniJson | undefined = undefined;
  @State private speechBytes: number = 0;
  @State private extracting: number = -1;   // 0..1 while unpacking, -1 when not
  @State private unpacked: boolean = false; // a previous extraction is still on disk

  private recorder: VoiceRecorder = new VoiceRecorder();
  private ticker: number = -1;

  private resolveTheme(): void {
    this.theme = resolvePalette(this.paletteFamily, this.isDark);
  }

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  /** Where pickBundle() extracts to, and where unpackedDir() looks for it. */
  private dest(): string {
    return this.ctx().filesDir + '/omni-model';
  }

  aboutToAppear(): void {
    this.resolveTheme();
    this.engine = yoai.nativeOmniAvailable();
    if (yoai.nativeOmniLoaded()) {
      this.modelName = '(already loaded)';
    }
    // An extraction from an earlier run, still on disk. Worth finding, because
    // the alternative is what this page used to do: forget the model existed
    // and make somebody re-copy 3.4 GB through the picker. App storage survives
    // a crash and an update, so after either of those the model is right there
    // and only this page's state was lost.
    this.unpacked = fs.accessSync(this.dest() + '/config.json');
  }

  aboutToDisappear(): void {
    if (this.ticker !== -1) {
      clearInterval(this.ticker);
      this.ticker = -1;
    }
    if (this.recorder.recording()) {
      this.recorder.discard();
    }
  }

  //----------------------------------------------------------------------------
  // Actions
  //----------------------------------------------------------------------------

  /**
   * Two ways in, because the model is a directory and a phone is bad at those.
   *
   *   Folder  -- the model already on the device, twenty-odd files intact.
   *   Bundle  -- ONE file, packed by tools/pack-mnn-model.py, extracted into
   *              this app's own storage. What to hand somebody who has to get
   *              3.4 GB onto a phone without losing a layout.
   *
   * Both end at the same place: a real directory MNN can resolve config.json
   * against. The bundle path costs disk twice over while it runs -- the copy
   * plus the extraction -- and the bundle can be deleted afterwards.
   */
  private async pickBundle(): Promise<void> {
    let uris: string[] = [];
    try {
      const opts: picker.DocumentSelectOptions = new picker.DocumentSelectOptions();
      opts.maxSelectNumber = 1;
      uris = await new picker.DocumentViewPicker(this.ctx()).select(opts);
    } catch (err) {
      const e = err as BusinessError;
      this.note = 'picker failed: ' + e.code + ' ' + e.message;
      return;
    }
    if (uris.length === 0) {
      return;
    }

    const src: string = new fileUri.FileUri(uris[0]).path;
    // Read the table first. A file that is not a bundle should say so now, not
    // after several minutes of copying.
    const listed: string = yoai.nativeBundleList(src);
    if (listed === '') {
      this.note = 'not a bundle (or truncated) — pack it with tools/pack-mnn-model.py';
      return;
    }

    // Into the app's own storage, which is the only place it can be sure of
    // writing and the only place that survives a reboot without a grant.
    const dest: string = this.dest();
    this.extracting = 0;
    this.busy = 'unpacking…';
    this.note = '';
    yoai.nativeBundleExtract(src, dest, (type: string, content: string) => {
      if (type === 'progress') {
        this.extracting = Number.parseFloat(content);
        return;
      }
      this.extracting = -1;
      if (type !== 'ok') {
        this.busy = '';
        this.note = content;
        return;
      }
      this.unpacked = true;
      this.load(dest, 'unpacked — the bundle in Download can be deleted now');
    });
  }

  /**
   * Load whatever a previous run already extracted.
   *
   * The one action on this page that costs nothing but the load itself. Without
   * it a crash means re-picking and re-writing several gigabytes of flash to
   * arrive at bytes that never left.
   */
  private loadUnpacked(): void {
    this.load(this.dest(), '');
  }

  /**
   * Change the diffusion step count for the next ask.
   *
   * Synchronous and cheap: MNN reads the value when it generates, so nothing is
   * reloaded and a sweep compares step counts rather than page caches — the
   * same reason the thread sweep on /asr does not reload either.
   */
  private setDitSteps(n: number): void {
    const rc: number = yoai.nativeOmniSetDitSteps(n);
    if (rc !== 0) {
      this.note = 'the engine would not take ' + n + ' DiT steps';
      return;
    }
    this.ditSteps = n;
    this.note = '';
  }

  /** Hand a directory to the engine. Shared by all three ways in. */
  private load(dir: string, okNote: string): void {
    this.busy = 'loading 3.4 GB…';
    this.note = '';
    yoai.nativeOmniLoad(dir, (type: string, content: string) => {
      this.busy = '';
      if (type === 'ok') {
        this.modelName = dir;
        this.note = okNote;
      } else {
        this.modelName = '';
        this.note = content;
      }
    });
  }

  /**
   * Pick the model DIRECTORY, for a model already laid out on the device.
   *
   * Folder mode, not file mode: MNN resolves the whole model relative to
   * config.json, so one file is not enough. This is also why omni cannot reuse
   * LocalModelStore, which is built around picking a single .gguf.
   */
  private async pickModel(): Promise<void> {
    let uris: string[] = [];
    try {
      const opts: picker.DocumentSelectOptions = new picker.DocumentSelectOptions();
      opts.selectMode = picker.DocumentSelectMode.FOLDER;
      opts.maxSelectNumber = 1;
      uris = await new picker.DocumentViewPicker(this.ctx()).select(opts);
    } catch (err) {
      const e = err as BusinessError;
      this.note = 'folder picker failed: ' + e.code + ' ' + e.message;
      return;
    }
    if (uris.length === 0) {
      return;
    }

    this.load(new fileUri.FileUri(uris[0]).path, '');
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
    this.reply = undefined;
    this.speechBytes = 0;
    this.note = '';
    this.recording = true;
    this.elapsed = 0;
    this.ticker = setInterval(() => {
      this.elapsed = this.recorder.seconds();
      // Stopped for you at the cliff rather than after it: the engine does not
      // refuse long audio gracefully, it crashes, and the core's own guard would
      // only report that the recording was wasted.
      if (this.elapsed >= OMNI_MAX_SECONDS) {
        this.finish();
      }
    }, 200);
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
    if (!yoai.nativeOmniLoaded()) {
      this.note = 'no model loaded — pick the model directory first';
      return;
    }

    this.busy = this.speak ? 'thinking, then speaking…' : 'thinking…';
    // A REPLY to the speaker, not a note about them.
    //
    // This used to ask 请用一句话说说这段音频里的人听起来怎么样 and got back
    // 他听起来很礼貌 — third person, an analyst describing a subject. Spoken
    // aloud by the phone that just listened, it is the model talking about the
    // person rather than to them.
    //
    // Two jobs had been merged into one prompt. Whether the model can read a
    // speaker at all belongs to the text axes, and docs/voice.md already
    // settles it: the 3B was measured not to hear tone, so anything it says
    // about mood comes from the words. What this page needs is speech, and
    // speech needs something worth saying to the person who spoke.
    yoai.nativeOmniAsk(pcm, '请直接用中文简短回应说话的人，不要描述或评价他。', this.speak,
      (type: string, content: string) => {
        this.busy = '';
        if (type !== 'result') {
          this.note = content;
          return;
        }
        this.reply = JSON.parse(content) as OmniJson;
        this.speechBytes = this.speak ? yoai.nativeOmniLastSpeech().byteLength : 0;
        this.note = '';
      });
  }

  /** Play back what the Talker said, at ITS rate rather than the recorder's. */
  private async playSpeech(): Promise<void> {
    const pcm: ArrayBuffer = yoai.nativeOmniLastSpeech();
    if (pcm.byteLength === 0) {
      this.note = 'no speech to play';
      return;
    }
    const streamInfo: audio.AudioStreamInfo = {
      samplingRate: audio.AudioSamplingRate.SAMPLE_RATE_24000,
      channels: audio.AudioChannel.CHANNEL_1,
      sampleFormat: audio.AudioSampleFormat.SAMPLE_FORMAT_S16LE,
      encodingType: audio.AudioEncodingType.ENCODING_TYPE_RAW
    };
    const rendererInfo: audio.AudioRendererInfo = {
      usage: audio.StreamUsage.STREAM_USAGE_VOICE_ASSISTANT,
      rendererFlags: 0
    };
    try {
      const renderer = await audio.createAudioRenderer({
        streamInfo: streamInfo,
        rendererInfo: rendererInfo
      });
      let at: number = 0;
      const view = new Uint8Array(pcm);
      renderer.on('writeData', (buffer: ArrayBuffer) => {
        const out = new Uint8Array(buffer);
        const n = Math.min(out.length, view.length - at);
        for (let i = 0; i < n; i++) {
          out[i] = view[at + i];
        }
        for (let i = n; i < out.length; i++) {
          out[i] = 0;
        }
        at += n;
        return audio.AudioDataCallbackResult.VALID;
      });
      await renderer.start();
    } catch (err) {
      const e = err as BusinessError;
      this.note = 'playback failed: ' + e.code + ' ' + e.message;
    }
  }

  //----------------------------------------------------------------------------
  // Pieces
  //----------------------------------------------------------------------------

  private fixed(v: number, places: number): string {
    return v.toFixed(places);
  }

  private reply2(): OmniJson {
    return this.reply as OmniJson;
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

      Text('Omni')
        .fontSize(20)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.theme.text_primary)
        .layoutWeight(1)
      Text(this.engine ? 'MNN on' : 'no MNN')
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
          .enabled(this.engine && this.busy === '')
          .onClick(() => { this.toggleRecording(); })

        Button('Folder')
          .height(44)
          .backgroundColor(this.theme.bg_surface)
          .fontColor(this.theme.text_primary)
          .enabled(this.engine && !this.recording && this.busy === '')
          .onClick(() => { this.pickModel(); })

        Button('Bundle')
          .height(44)
          .backgroundColor(this.theme.bg_surface)
          .fontColor(this.theme.text_primary)
          .enabled(this.engine && !this.recording && this.busy === '')
          .onClick(() => { this.pickBundle(); })
      }
      .width('100%')

      // Only when there is something to reuse and nothing loaded. After a crash
      // this is the button to press: the model is already in app storage and
      // re-picking the bundle would copy 3.4 GB to reach the same files.
      if (this.unpacked && this.modelName === '') {
        Button('Load the unpacked model')
          .width('100%')
          .height(40)
          .backgroundColor(this.theme.bg_surface)
          .fontColor(this.theme.text_primary)
          .fontSize(14)
          .enabled(this.engine && !this.recording && this.busy === '')
          .onClick(() => { this.loadUnpacked(); })
      }

      if (this.extracting >= 0) {
        Progress({ value: this.extracting * 100, total: 100, type: ProgressType.Linear })
          .width('100%')
          .color(this.theme.brand)
      }

      Row({ space: 8 }) {
        Toggle({ type: ToggleType.Switch, isOn: this.speak })
          .enabled(!this.recording && this.busy === '')
          .onChange((on: boolean) => { this.speak = on; })
        Text('speak the reply — this is the expensive half')
          .fontSize(12)
          .fontColor(this.theme.text_secondary)
          .layoutWeight(1)
      }
      .width('100%')

      // The diffusion sampler's step count, swept like the thread count on /asr.
      //
      // MNN runs steps-1 DiT forwards per chunk, so these are four, two and one.
      // It is the only lever on 3.7x that does not need a different model — and
      // it spends exactly what the model is here for, because a coarser sampler
      // is a less natural voice. So this is a two-handed measurement: the number
      // on screen and what it sounds like, together.
      if (this.modelName !== '') {
        Row({ space: 6 }) {
          Text('DiT steps')
            .fontSize(12)
            .fontColor(this.theme.text_secondary)
          ForEach([5, 3, 2], (n: number) => {
            Button(n.toString())
              .height(32)
              .layoutWeight(1)
              .fontSize(13)
              .backgroundColor(this.ditSteps === n ? this.theme.brand : this.theme.bg_surface)
              .fontColor(this.ditSteps === n ? this.theme.on_brand : this.theme.text_primary)
              .enabled(!this.recording && this.busy === '')
              .onClick(() => { this.setDitSteps(n); })
          }, (n: number) => n.toString())
        }
        .width('100%')
      }

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
    if (this.busy !== '')  { return this.busy; }
    if (this.recording)    { return 'recording ' + this.fixed(this.elapsed, 1) +
                                    ' s — stops itself at ' + OMNI_MAX_SECONDS; }
    if (this.modelName === '') {
      return this.engine ? 'no model — pick the MNN model folder' : 'built without MNN';
    }
    return 'model loaded · 16 kHz in, 24 kHz out';
  }

  @Builder
  result() {
    Column({ space: 10 }) {
      Text('What it said')
        .fontSize(15)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.theme.text_primary)
        .width('100%')

      Text(this.reply2().text === '' ? '(nothing)' : this.reply2().text)
        .fontSize(13)
        .fontColor(this.theme.text_primary)
        .width('100%')

      // The number this page exists for, on its own line and named plainly.
      if (this.reply2().speechSeconds > 0) {
        Column({ space: 4 }) {
          Text(this.fixed(this.reply2().speechRealtime, 2) + '× real time')
            .fontSize(22)
            .fontWeight(FontWeight.Medium)
            .fontColor(this.reply2().speechRealtime > 1
              ? this.theme.danger : this.theme.success)
          Text('seconds of computation per second of speech. Above 1 it cannot ' +
               'answer aloud as fast as it talks.')
            .fontSize(11)
            .fontColor(this.theme.text_tertiary)
        }
        .alignItems(HorizontalAlign.Start)
        .width('100%')

        Text('spoke ' + this.fixed(this.reply2().speechSeconds, 1) + ' s in ' +
             this.fixed(this.reply2().speechElapsed, 1) + ' s · ' +
             (this.speechBytes / 2 / SPEECH_SAMPLE_RATE).toFixed(1) + ' s buffered')
          .fontSize(11)
          .fontColor(this.theme.text_tertiary)
          .width('100%')

        // The ratio above is the cost of the WHOLE reply, and nobody waits for
        // a whole reply — they wait for the first word. If that comes early and
        // the blocks keep ahead of playback, the ratio is never experienced.
        // One block means nothing streamed and there is nothing to keep ahead.
        Text('first audio at ' + this.fixed(this.reply2().firstAudioSeconds, 1) +
             ' s, in ' + this.reply2().audioBlocks + ' block' +
             (this.reply2().audioBlocks === 1 ? ' — nothing streamed' : 's') +
             ' · thinker ' + this.fixed(this.reply2().thinkerSeconds, 1) + ' s')
          .fontSize(11)
          .fontColor(this.theme.text_tertiary)
          .width('100%')

        Button('Play it')
          .height(40)
          .backgroundColor(this.theme.bg_surface)
          .fontColor(this.theme.text_primary)
          .onClick(() => { this.playSpeech(); })
      }

      Text('whole exchange ' + this.fixed(this.reply2().seconds, 1) + ' s for ' +
           this.fixed(this.reply2().audioSeconds, 1) + ' s of audio')
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')

      // Said here rather than left for someone to infer from a good-looking
      // sentence: this model was measured not to hear tone.
      Text('Its read of the speaker comes from the WORDS, not the sound. Given ' +
           'the same sentence at three tape speeds it answered the same way ' +
           'every time, so anything here about mood is the text axes\' job, not ' +
           'this page\'s finding.')
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
          if (this.reply !== undefined) {
            this.result()
          } else if (this.busy === '') {
            Text('Record up to ' + OMNI_MAX_SECONDS + ' seconds and the model ' +
                 'answers — aloud, if the switch is on. The figure that matters ' +
                 'is how long the speaking takes against how long it lasts.')
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
