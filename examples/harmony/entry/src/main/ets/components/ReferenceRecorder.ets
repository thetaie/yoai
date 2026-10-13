/**
 * Recording a reference voice: read a passage aloud, listen back, use it.
 *
 * A port of examples/desktop/src/renderer/src/components/VoiceRecorder.tsx. A
 * recording anchors better than a synthesized reference -- one real speaker held
 * the pieces of a reply to 1.31 semitones on average, a clone of one to 1.68
 * (src/voice/tts/reader.hpp) -- so this is the first way offered to make a voice
 * of one's own. The passage is plain, even prose, because what the embedding
 * captures is how the voice sounds, not what it says.
 *
 * Captured at 24 kHz, mono, 16-bit, which is exactly what the engine takes; the
 * take is kept as a WAV under filesDir/recordings and then used exactly as a
 * picked .wav is: extracted once for the model size, cached, applied at once.
 */
import { common } from '@kit.AbilityKit';
import { DEFAULT_FAMILY, Palette, resolvePalette } from '../core/Palettes';
import { Reader } from '../core/Reader';
import { player } from '../core/SpeechPlayer';
import { REFERENCE_SAMPLE_RATE, VoiceRecorder } from '../core/VoiceRecorder';
import { saveRecording } from '../core/Wav';

/**
 * Below this a take is not offered for use. The references that were measured
 * were 17 and 22 seconds long; nothing shorter has been tried, so this is a
 * floor against an accidental tap rather than a measured limit.
 */
const MIN_SECONDS: number = 10;

/** At or above this the take has hit full scale somewhere, and is clipped. */
const CLIP: number = 0.99;

@Component
export struct ReferenceRecorder {
  /** The take is now the reference voice. */
  onUsed: () => void = () => {};
  onClose: () => void = () => {};

  @StorageProp('paletteFamily') @Watch('resolveTheme') paletteFamily: string = DEFAULT_FAMILY;
  @StorageProp('isDark') @Watch('resolveTheme') isDark: boolean = false;
  @State private theme: Palette = resolvePalette(DEFAULT_FAMILY, false);

  /** 'idle' | 'recording' | 'recorded' | 'saving' */
  @State private phase: string = 'idle';
  @State private seconds: number = 0;
  @State private level: number = 0;
  @State private clipped: boolean = false;
  @State private error: ResourceStr = '';

  private recorder: VoiceRecorder = new VoiceRecorder(REFERENCE_SAMPLE_RATE);
  private pcm: ArrayBuffer | null = null;
  private ticker: number = -1;

  private resolveTheme(): void {
    this.theme = resolvePalette(this.paletteFamily, this.isDark);
  }

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  aboutToAppear(): void {
    this.resolveTheme();
  }

  /** Leaving the page mid-take lets go of the microphone. */
  aboutToDisappear(): void {
    this.stopTicker();
    if (this.recorder.recording()) {
      this.recorder.discard();
    }
    player.stop();
  }

  private stopTicker(): void {
    if (this.ticker !== -1) {
      clearInterval(this.ticker);
      this.ticker = -1;
    }
  }

  private async start(): Promise<void> {
    this.error = '';
    player.stop();
    if (!await VoiceRecorder.requestPermission(this.ctx())) {
      this.error = $r('app.string.record_denied');
      return;
    }
    if (!await this.recorder.start()) {
      this.error = $r('app.string.record_no_mic');
      return;
    }
    this.pcm = null;
    this.seconds = 0;
    this.level = 0;
    this.clipped = false;
    this.phase = 'recording';
    this.ticker = setInterval(() => {
      this.seconds = this.recorder.seconds();
      this.level = this.recorder.level();
      this.clipped = this.recorder.peak() >= CLIP;
    }, 100);
  }

  private async stop(): Promise<void> {
    this.stopTicker();
    const wasClipped: boolean = this.recorder.peak() >= CLIP;
    const pcm: ArrayBuffer = await this.recorder.stop();
    this.pcm = pcm;
    this.seconds = pcm.byteLength / 2 / REFERENCE_SAMPLE_RATE;
    this.level = 0;
    this.clipped = wasClipped;
    this.phase = 'recorded';
  }

  private play(): void {
    const pcm: ArrayBuffer | null = this.pcm;
    if (pcm !== null) {
      player.stop();
      player.push(new Int16Array(pcm));
      player.flush();
    }
  }

  private async use(): Promise<void> {
    const pcm: ArrayBuffer | null = this.pcm;
    if (pcm === null) {
      return;
    }
    player.stop();
    this.error = '';
    this.phase = 'saving';
    let why: string = '';
    try {
      const path: string = saveRecording(this.ctx(), pcm, REFERENCE_SAMPLE_RATE);
      const name: string = path.substring(path.lastIndexOf('/') + 1);
      why = await Reader.setReference({ uri: '', path: path, name: name });
    } catch (err) {
      why = (err as Error).message;
    }
    this.phase = 'recorded';
    if (why.length > 0) {
      this.error = why;
      return;
    }
    this.onUsed();
  }

  private clock(): string {
    const s: number = Math.floor(this.seconds);
    return Math.floor(s / 60) + ':' + (s % 60 < 10 ? '0' : '') + (s % 60);
  }

  build() {
    Column({ space: 10 }) {
      Text($r('app.string.record_title'))
        .fontSize(14)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.theme.text_primary)
        .width('100%')
      Text($r('app.string.record_script'))
        .fontSize(15)
        .lineHeight(24)
        .fontColor(this.theme.text_primary)
        .width('100%')
        .padding(10)
        .borderRadius(8)
        .backgroundColor(this.theme.bg_page)
      Text($r('app.string.record_note'))
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')

      Row({ space: 10 }) {
        Text(this.clock())
          .fontSize(15)
          .fontFamily('monospace')
          .fontColor(this.theme.text_primary)
        if (this.phase === 'recording') {
          Stack({ alignContent: Alignment.Start }) {
            Column()
              .width(Math.min(100, this.level * 400) + '%')
              .height(6)
              .borderRadius(3)
              .backgroundColor(this.clipped ? this.theme.danger : this.theme.brand)
          }
          .layoutWeight(1)
          .height(6)
          .borderRadius(3)
          .backgroundColor(this.theme.bg_page)
        }
      }
      .width('100%')

      Flex({ wrap: FlexWrap.Wrap }) {
        if (this.phase === 'recording') {
          Button($r('app.string.record_stop'))
            .height(34)
            .fontSize(13)
            .margin({ right: 8, bottom: 6 })
            .backgroundColor(this.theme.danger)
            .fontColor(this.theme.on_brand)
            .onClick(() => { this.stop(); })
        } else {
          if (this.phase !== 'idle') {
            Button($r('app.string.record_play'))
              .height(34)
              .fontSize(13)
              .margin({ right: 8, bottom: 6 })
              .enabled(this.phase !== 'saving')
              .backgroundColor(this.theme.bg_page)
              .fontColor(this.theme.text_primary)
              .onClick(() => { this.play(); })
          }
          Button(this.phase === 'idle' ? $r('app.string.record_start') : $r('app.string.record_again'))
            .height(34)
            .fontSize(13)
            .margin({ right: 8, bottom: 6 })
            .enabled(this.phase !== 'saving')
            .backgroundColor(this.phase === 'idle' ? this.theme.brand : this.theme.bg_page)
            .fontColor(this.phase === 'idle' ? this.theme.on_brand : this.theme.text_primary)
            .onClick(() => { this.start(); })
          if (this.phase !== 'idle') {
            Button(this.phase === 'saving' ? $r('app.string.record_saving') : $r('app.string.record_use'))
              .height(34)
              .fontSize(13)
              .margin({ right: 8, bottom: 6 })
              .enabled(this.phase !== 'saving' && this.seconds >= MIN_SECONDS)
              .backgroundColor(this.theme.brand)
              .fontColor(this.theme.on_brand)
              .onClick(() => { this.use(); })
          }
        }
        Button($r('app.string.settings_cancel'))
          .height(34)
          .fontSize(13)
          .margin({ bottom: 6 })
          .enabled(this.phase !== 'saving')
          .backgroundColor(this.theme.bg_page)
          .fontColor(this.theme.text_secondary)
          .onClick(() => {
            if (this.recorder.recording()) {
              this.stopTicker();
              this.recorder.discard();
            }
            this.onClose();
          })
      }
      .width('100%')

      if ((this.phase === 'recorded' || this.phase === 'saving') && this.seconds < MIN_SECONDS) {
        Text($r('app.string.record_too_short'))
          .fontSize(11)
          .fontColor(this.theme.danger)
          .width('100%')
      }
      if (this.clipped) {
        Text($r('app.string.record_clipped'))
          .fontSize(11)
          .fontColor(this.theme.danger)
          .width('100%')
      }
      if (this.error !== '') {
        Text(this.error)
          .fontSize(11)
          .fontColor(this.theme.danger)
          .width('100%')
      }
    }
    .width('100%')
    .padding(16)
    .borderRadius(12)
    .backgroundColor(this.theme.bg_surface)
  }
}
