/**
 * Speech output: which model files, which voice, and how replies are read.
 *
 * The desktop's 语音输出 section on a page of its own, and the same order: the
 * voice mode first, because it decides which talker the rest is about; then
 * the files that mode still needs and the two ways to get them; then the voice
 * itself; then how the voice loaded; then the reading settings.
 *
 * There is no load button. The voice loads by itself once both files of the
 * mode are there, at startup and after any change here, and the status line
 * says how that went -- a button pressed every launch is a step that exists
 * only to be forgotten.
 *
 * Getting the files differs from the desktop in one way. There, picking one
 * file fills the others from its folder; here a picked file's grant does not
 * reach its neighbours, so 选择已有文件 takes several files at once and reads
 * each one's slot from its name.
 */
import { common } from '@kit.AbilityKit';
import { router } from '@kit.ArkUI';
import { ReferenceRecorder } from '../components/ReferenceRecorder';
import { DownloadState, formatBytes } from '../core/Download';
import { ICON_ROW, ICON_TOOLBAR, icon } from '../core/Icons';
import { LocalModelStore, PickedFile } from '../core/LocalModelStore';
import { ModelDownloads } from '../core/ModelDownloads';
import { SpeechSlot, slotOf, speechFile } from '../core/ModelFiles';
import { ModelStore } from '../core/ModelStore';
import { DEFAULT_FAMILY, Palette, resolvePalette } from '../core/Palettes';
import { READER_REV, Reader, ReaderStatus } from '../core/Reader';
import { Speak, SPEAKING_ID } from '../core/Speak';
import { player, SPEECH_ACTIVE } from '../core/SpeechPlayer';
import { DEFAULT_TEMPERATURE, FileRef, SpeechSettings, SpeechStore } from '../core/SpeechStore';

/** One CustomVoice style: its stored value and its label. */
interface StyleRow {
  value: string;
  label: Resource;
}

const STYLES: StyleRow[] = [
  { value: '', label: $r('app.string.tts_style_none') },
  { value: 'calm', label: $r('app.string.tts_style_calm') },
  { value: 'caring', label: $r('app.string.tts_style_caring') },
  { value: 'cheerful', label: $r('app.string.tts_style_cheerful') },
  { value: 'sorry', label: $r('app.string.tts_style_sorry') }
];

/** The id the test utterance is spoken under, so its button can follow it. */
const TEST_ID: string = 'settings-test';

@Entry
@Component
struct SettingsSpeech {
  @StorageLink('paletteFamily') @Watch('resolveTheme') paletteFamily: string = DEFAULT_FAMILY;
  @StorageProp('isDark') @Watch('resolveTheme') isDark: boolean = false;
  @State private theme: Palette = resolvePalette(DEFAULT_FAMILY, false);

  @StorageProp(READER_REV) @Watch('refresh') readerRev: number = 0;
  @StorageProp(SPEECH_ACTIVE) speaking: boolean = false;
  @StorageProp(SPEAKING_ID) speakingId: string = '';

  @State private status: ReaderStatus = Reader.status();
  @State private settings: SpeechSettings = SpeechStore.get();
  @State private missing: SpeechSlot[] = [];
  @State private bundled: string[] = [];
  @State private download: DownloadState = ModelDownloads.speech.status();
  @State private recording: boolean = false;
  /** A file being picked, or a reference voice being applied. */
  @State private busy: boolean = false;
  @State private note: string = '';
  @State private noteDanger: boolean = false;

  // The three number fields, as typed until they are saved.
  @State private minChars: string = '';
  @State private seed: string = '';
  @State private temperature: string = '';

  private poller: number = -1;

  private resolveTheme(): void {
    this.theme = resolvePalette(this.paletteFamily, this.isDark);
  }

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  aboutToAppear(): void {
    this.resolveTheme();   // @Watch does not fire on construction; see Index
    this.bundled = Reader.bundledVoices();
    this.refresh();
    const s: SpeechSettings = SpeechStore.get();
    this.minChars = String(s.minChars);
    this.seed = String(s.seed);
    this.temperature = String(s.temperature);
    if (ModelDownloads.speech.status().running) {
      this.follow();
    }
  }

  aboutToDisappear(): void {
    if (this.poller !== -1) {
      clearInterval(this.poller);
      this.poller = -1;
    }
  }

  /** Re-read everything this page shows; READER_REV calls it whenever the voice changes. */
  private refresh(): void {
    this.status = Reader.status();
    this.settings = SpeechStore.get();
    this.missing = SpeechStore.missingSlots();
  }

  private say(text: string, danger: boolean): void {
    this.note = text;
    this.noteDanger = danger;
  }

  //----------------------------------------------------------------------------
  // Files
  //----------------------------------------------------------------------------

  private async setMode(mode: string): Promise<void> {
    if (mode === this.settings.voiceMode) {
      return;
    }
    await SpeechStore.setVoiceMode(this.ctx(), mode);
    this.refresh();
    // The other talker: what is loaded is the wrong one now.
    Reader.reload();
  }

  private async pickSlot(slot: SpeechSlot): Promise<void> {
    const picked: PickedFile | undefined = await LocalModelStore.pickGguf(this.ctx());
    if (picked === undefined) {
      return;
    }
    await SpeechStore.setFile(this.ctx(), slot, { uri: picked.uri, path: picked.path, name: picked.name });
    this.refresh();
    Reader.reload();
  }

  /**
   * Pick any of the files, several at once. Each goes to the slot its name
   * says; one whose name says nothing fills the first slot still missing. A
   * wrong guess costs little: the load names the file it refused.
   */
  private async pickExisting(): Promise<void> {
    const files: PickedFile[] = await LocalModelStore.pickFiles(this.ctx(), ['.gguf'], 3);
    if (files.length === 0) {
      return;
    }
    const open: SpeechSlot[] = SpeechStore.missingSlots();
    for (const f of files) {
      const named: string = slotOf(f.name);
      let slot: SpeechSlot | undefined = undefined;
      if (named === 'talker' || named === 'customTalker' || named === 'tokenizer') {
        slot = named as SpeechSlot;
      } else if (open.length > 0) {
        slot = open[0];
      }
      if (slot === undefined) {
        continue;
      }
      const i: number = open.indexOf(slot);
      if (i >= 0) {
        open.splice(i, 1);
      }
      await SpeechStore.setFile(this.ctx(), slot, { uri: f.uri, path: f.path, name: f.name });
    }
    this.refresh();
    Reader.reload();
  }

  private async startDownload(): Promise<void> {
    if (await ModelDownloads.downloadSpeech(this.ctx(), SpeechStore.missingSlots())) {
      this.follow();
    }
  }

  /** Poll the download while it runs; it runs outside this page. */
  private follow(): void {
    if (this.poller !== -1) {
      return;
    }
    this.download = ModelDownloads.speech.status();
    this.poller = setInterval(() => {
      const d: DownloadState = ModelDownloads.speech.status();
      this.download = d;
      if (!d.running) {
        clearInterval(this.poller);
        this.poller = -1;
        this.refresh();
      }
    }, 500);
  }

  private missingBytes(): number {
    let n: number = 0;
    for (const slot of this.missing) {
      n += speechFile(slot).size;
    }
    return n;
  }

  private slotPath(slot: SpeechSlot): string {
    const s: SpeechSettings = this.settings;
    const ref: FileRef | null = slot === 'talker' ? s.talker : slot === 'customTalker' ? s.customTalker : s.tokenizer;
    return ref === null ? '' : ref.path;
  }

  private talkerSlot(): SpeechSlot {
    return this.settings.voiceMode === 'custom' ? 'customTalker' : 'talker';
  }

  //----------------------------------------------------------------------------
  // The voice
  //----------------------------------------------------------------------------

  private async chooseBundled(name: string): Promise<void> {
    this.busy = true;
    const why: string = await Reader.useBundled(name);
    this.busy = false;
    this.refresh();
    this.say(why, why.length > 0);
  }

  private async pickReference(): Promise<void> {
    const files: PickedFile[] = await LocalModelStore.pickFiles(this.ctx(), ['.wav', '.spk'], 1);
    if (files.length === 0) {
      return;
    }
    this.busy = true;
    const f: PickedFile = files[0];
    const why: string = await Reader.setReference({ uri: f.uri, path: f.path, name: f.name });
    this.busy = false;
    this.refresh();
    this.say(why, why.length > 0);
  }

  /** The bundled voice in use, '' while a file of the user's is. */
  private bundledInUse(): string {
    return this.settings.reference === null ? Reader.bundledName() : '';
  }

  /** The speaker in force: the stored one if the talker has it, else serena, else its first. */
  private speakerInUse(): string {
    const names: string[] = this.status.speakers;
    const chosen: string = this.settings.speaker;
    return names.includes(chosen) ? chosen
      : names.includes('serena') ? 'serena'
      : names.length > 0 ? names[0] : '';
  }

  private async setSpeaker(name: string): Promise<void> {
    await SpeechStore.setSpeaker(this.ctx(), name);
    this.refresh();
  }

  private async setStyle(style: string): Promise<void> {
    await SpeechStore.setStyle(this.ctx(), style);
    this.refresh();
  }

  //----------------------------------------------------------------------------
  // Reading
  //----------------------------------------------------------------------------

  private async setEnabled(on: boolean): Promise<void> {
    await SpeechStore.setEnabled(this.ctx(), on);
    this.refresh();
    // Reading adds the spoken-style paragraph to the prompt, or drops it.
    ModelStore.warmActive(this.ctx());
  }

  private async saveNumbers(): Promise<void> {
    await SpeechStore.setMinChars(this.ctx(), Number.parseInt(this.minChars, 10));
    await SpeechStore.setSeed(this.ctx(), Number.parseInt(this.seed, 10));
    if (this.settings.voiceMode !== 'custom') {
      await SpeechStore.setTemperature(this.ctx(), Number.parseFloat(this.temperature));
    }
    const s: SpeechSettings = SpeechStore.get();
    this.minChars = String(s.minChars);
    this.seed = String(s.seed);
    this.temperature = String(s.temperature);
    this.refresh();
  }

  private async test(): Promise<void> {
    this.say('', false);
    const text: string = this.ctx().resourceManager.getStringSync($r('app.string.settings_tts_test_text').id);
    const reason: string = await Speak.speak(TEST_ID, text);
    if (reason.length > 0) {
      this.say(reason, true);
      return;
    }
    // A stream that started can still fail, and says so when it ends.
    Speak.watchOnce().then((err: string) => {
      if (err.length > 0) {
        this.say(err, true);
      }
    });
  }

  /** The line saying how the voice loaded. */
  private statusLine(): ResourceStr {
    const st: ReaderStatus = this.status;
    if (st.extracting) {
      return $r('app.string.settings_tts_extracting');
    }
    if (st.loading) {
      return $r('app.string.settings_tts_loading');
    }
    if (st.loaded) {
      return $r('app.string.settings_tts_loaded', st.modelType);
    }
    if (st.note.length > 0) {
      return st.note;
    }
    if (this.slotPath(this.talkerSlot()).length === 0 || this.slotPath('tokenizer').length === 0) {
      return $r('app.string.settings_tts_pick_both');
    }
    return $r('app.string.settings_tts_loading');
  }

  private downloadError(): ResourceStr {
    const e: string = this.download.error;
    if (e === 'space') {
      return $r('app.string.download_err_space');
    }
    if (e === 'network') {
      return $r('app.string.download_err_network');
    }
    if (e === 'checksum') {
      return $r('app.string.download_err_checksum');
    }
    if (e === 'disk') {
      return $r('app.string.download_err_disk');
    }
    return $r('app.string.download_err_canceled');
  }

  //----------------------------------------------------------------------------
  // Views
  //----------------------------------------------------------------------------

  @Builder
  sectionTitle(text: ResourceStr) {
    Text(text)
      .fontSize(13)
      .fontColor(this.theme.text_tertiary)
      .width('100%')
      .padding({ left: 16, right: 16, top: 18, bottom: 6 })
  }

  @Builder
  tick() {
    icon('check', ICON_ROW, this.theme.brand)
  }

  @Builder
  modeRows() {
    Column({ space: 8 }) {
      Row({ space: 12 }) {
        Text($r('app.string.settings_tts_mode_base'))
          .fontSize(15)
          .fontColor(this.theme.text_primary)
          .layoutWeight(1)
        if (this.settings.voiceMode !== 'custom') {
          this.tick()
        }
      }
      .width('100%')
      .height(44)
      .onClick(() => { this.setMode('base'); })

      Row({ space: 12 }) {
        Text($r('app.string.settings_tts_mode_custom'))
          .fontSize(15)
          .fontColor(this.theme.text_primary)
          .layoutWeight(1)
        if (this.settings.voiceMode === 'custom') {
          this.tick()
        }
      }
      .width('100%')
      .height(44)
      .onClick(() => { this.setMode('custom'); })

      Text($r('app.string.settings_tts_mode_note'))
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')
    }
    .width('100%')
    .padding(16)
    .margin({ left: 12, right: 12 })
    .borderRadius(12)
    .backgroundColor(this.theme.bg_surface)
  }

  /** What a new user sees first: the files this mode still needs, and two ways to get them. */
  @Builder
  setupCard() {
    Column({ space: 8 }) {
      if (this.download.running) {
        Text(this.download.retry > 0
          ? $r('app.string.download_retrying', this.download.retryIn, this.download.retry,
               this.download.retries)
          : this.download.file.length === 0
            ? $r('app.string.download_connecting')
            : this.download.verifying
              ? $r('app.string.download_verifying', this.download.file)
              : $r('app.string.download_downloading', this.download.file))
          .fontSize(13)
          .fontColor(this.theme.text_primary)
          .width('100%')
        Progress({ value: this.download.received, total: Math.max(this.download.total, 1),
                   type: ProgressType.Linear })
          .width('100%')
          .color(this.theme.brand)
        Row({ space: 8 }) {
          Text(formatBytes(this.download.received) + ' / ' + formatBytes(this.download.total) +
               (this.download.host.length > 0 ? ' · ' + this.download.host : ''))
            .fontSize(11)
            .fontColor(this.theme.text_tertiary)
            .layoutWeight(1)
          Button($r('app.string.settings_cancel'))
            .height(30)
            .fontSize(12)
            .backgroundColor(this.theme.bg_page)
            .fontColor(this.theme.text_primary)
            .onClick(() => { ModelDownloads.speech.cancel(); })
        }
        .width('100%')
      } else {
        Text($r('app.string.settings_tts_setup'))
          .fontSize(13)
          .fontColor(this.theme.text_primary)
          .width('100%')
        ForEach(this.missing, (slot: SpeechSlot) => {
          Text(speechFile(slot).name + ' · ' + formatBytes(speechFile(slot).size))
            .fontSize(12)
            .fontColor(this.theme.text_secondary)
            .width('100%')
        }, (slot: SpeechSlot) => slot)
        Row({ space: 8 }) {
          Button($r('app.string.settings_tts_pick_existing'))
            .height(36)
            .fontSize(13)
            .backgroundColor(this.theme.bg_page)
            .fontColor(this.theme.text_primary)
            .onClick(() => { this.pickExisting(); })
          Button($r('app.string.download_button', formatBytes(this.missingBytes())))
            .height(36)
            .fontSize(13)
            .backgroundColor(this.theme.brand)
            .fontColor(this.theme.on_brand)
            .onClick(() => { this.startDownload(); })
        }
        Text(LocalModelStore.folderPickable()
          ? $r('app.string.settings_tts_setup_note_folder')
          : $r('app.string.settings_tts_setup_note_app'))
          .fontSize(11)
          .fontColor(this.theme.text_tertiary)
          .width('100%')
        if (this.download.error.length > 0) {
          Text(this.downloadError())
            .fontSize(11)
            .fontColor(this.download.error === 'canceled' ? this.theme.text_tertiary : this.theme.danger)
            .width('100%')
          if (this.download.detail.length > 0) {
            Text(this.download.detail)
              .fontSize(11)
              .fontColor(this.theme.text_tertiary)
              .width('100%')
          }
        }
      }
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding(16)
    .margin({ left: 12, right: 12, top: 12 })
    .borderRadius(12)
    .backgroundColor(this.theme.bg_surface)
  }

  /**
   * The two files the mode loads, each with where it is. A row offers 更换 only
   * once it has a file: a missing one is got through the card above, and a 选择
   * here as well was the same action twice on one screen.
   */
  @Builder
  slotsCard() {
    Column() {
      Row({ space: 12 }) {
        Column({ space: 2 }) {
          Text(this.settings.voiceMode === 'custom'
            ? $r('app.string.settings_tts_talker_custom')
            : $r('app.string.settings_tts_talker'))
            .fontSize(15)
            .fontColor(this.theme.text_primary)
            .width('100%')
          Text(this.slotPath(this.talkerSlot()).length > 0
            ? this.slotPath(this.talkerSlot())
            : $r('app.string.settings_tts_not_picked'))
            .fontSize(11)
            .fontColor(this.theme.text_tertiary)
            .width('100%')
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        if (this.slotPath(this.talkerSlot()).length > 0) {
          Button($r('app.string.settings_tts_replace'))
            .height(32)
            .fontSize(13)
            .enabled(!this.download.running)
            .backgroundColor(this.theme.bg_page)
            .fontColor(this.theme.text_primary)
            .onClick(() => { this.pickSlot(this.talkerSlot()); })
        }
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 12, bottom: 12 })

      Divider().color(this.theme.divider).margin({ left: 16, right: 16 })

      Row({ space: 12 }) {
        Column({ space: 2 }) {
          Text($r('app.string.settings_tts_tokenizer'))
            .fontSize(15)
            .fontColor(this.theme.text_primary)
            .width('100%')
          Text(this.slotPath('tokenizer').length > 0
            ? this.slotPath('tokenizer')
            : $r('app.string.settings_tts_not_picked'))
            .fontSize(11)
            .fontColor(this.theme.text_tertiary)
            .width('100%')
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        if (this.slotPath('tokenizer').length > 0) {
          Button($r('app.string.settings_tts_replace'))
            .height(32)
            .fontSize(13)
            .enabled(!this.download.running)
            .backgroundColor(this.theme.bg_page)
            .fontColor(this.theme.text_primary)
            .onClick(() => { this.pickSlot('tokenizer'); })
        }
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 12, bottom: 12 })
    }
    .width('100%')
    .margin({ left: 12, right: 12, top: 12 })
    .borderRadius(12)
    .backgroundColor(this.theme.bg_surface)
  }

  /**
   * Base: the voice every piece is anchored to -- a bundled one, a recording,
   * or a file. The bundled ones are listed so a new user has a voice without
   * first finding or recording one.
   */
  @Builder
  referenceCard() {
    Column({ space: 8 }) {
      ForEach(this.bundled, (name: string) => {
        Row({ space: 12 }) {
          Text(name)
            .fontSize(15)
            .fontColor(this.theme.text_primary)
            .layoutWeight(1)
          if (this.bundledInUse() === name) {
            this.tick()
          }
        }
        .width('100%')
        .height(40)
        .enabled(!this.busy)
        .onClick(() => { this.chooseBundled(name); })
      }, (name: string) => name)

      if (this.settings.reference !== null) {
        Row({ space: 12 }) {
          Column({ space: 2 }) {
            Text($r('app.string.settings_reader_ref_own'))
              .fontSize(15)
              .fontColor(this.theme.text_primary)
              .width('100%')
            Text((this.settings.reference as FileRef).path)
              .fontSize(11)
              .fontColor(this.theme.text_tertiary)
              .width('100%')
          }
          .alignItems(HorizontalAlign.Start)
          .layoutWeight(1)
          this.tick()
        }
        .width('100%')
      }

      Row({ space: 8 }) {
        Button($r('app.string.settings_reader_ref_record'))
          .height(34)
          .fontSize(13)
          .enabled(!this.busy && !this.recording)
          .backgroundColor(this.theme.bg_page)
          .fontColor(this.theme.text_primary)
          .onClick(() => { this.recording = true; })
        Button(this.busy || this.status.extracting
          ? $r('app.string.settings_reader_ref_busy')
          : $r('app.string.settings_reader_ref_pick'))
          .height(34)
          .fontSize(13)
          .enabled(!this.busy && !this.status.extracting)
          .backgroundColor(this.theme.bg_page)
          .fontColor(this.theme.text_primary)
          .onClick(() => { this.pickReference(); })
      }

      Text($r('app.string.settings_reader_ref_note'))
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding(16)
    .margin({ left: 12, right: 12 })
    .borderRadius(12)
    .backgroundColor(this.theme.bg_surface)
  }

  /** CustomVoice: one of the talker's own speakers, and a style to read in. */
  @Builder
  speakerCard() {
    Column({ space: 4 }) {
      if (this.status.speakers.length === 0) {
        Text($r('app.string.settings_tts_speaker_note'))
          .fontSize(12)
          .fontColor(this.theme.text_tertiary)
          .width('100%')
      }
      Flex({ wrap: FlexWrap.Wrap }) {
        ForEach(this.status.speakers, (name: string) => {
          Button(name)
            .height(32)
            .fontSize(13)
            .margin({ right: 6, bottom: 6 })
            .backgroundColor(this.speakerInUse() === name ? this.theme.brand : this.theme.bg_page)
            .fontColor(this.speakerInUse() === name ? this.theme.on_brand : this.theme.text_primary)
            .onClick(() => { this.setSpeaker(name); })
        }, (name: string) => name)
      }
      .width('100%')

      Text($r('app.string.settings_tts_style'))
        .fontSize(12)
        .fontColor(this.theme.text_tertiary)
        .width('100%')
        .margin({ top: 8 })
      ForEach(STYLES, (s: StyleRow) => {
        Row({ space: 12 }) {
          Text(s.label)
            .fontSize(15)
            .fontColor(this.theme.text_primary)
            .layoutWeight(1)
          if (this.settings.style === s.value) {
            this.tick()
          }
        }
        .width('100%')
        .height(40)
        .onClick(() => { this.setStyle(s.value); })
      }, (s: StyleRow) => 'style-' + s.value)
      Text($r('app.string.settings_tts_style_note'))
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding(16)
    .margin({ left: 12, right: 12 })
    .borderRadius(12)
    .backgroundColor(this.theme.bg_surface)
  }

  /** How the voice loaded, the test, and the reading settings. */
  @Builder
  readingCard() {
    Column({ space: 10 }) {
      Row({ space: 8 }) {
        Text(this.statusLine())
          .fontSize(13)
          .fontColor(this.status.loaded ? this.theme.success
            : this.status.note.length > 0 ? this.theme.danger : this.theme.text_secondary)
          .layoutWeight(1)
        if (this.status.loaded) {
          Button(this.speakingId === TEST_ID
            ? $r('app.string.action_stop_speaking')
            : $r('app.string.settings_tts_test'))
            .height(32)
            .fontSize(13)
            .backgroundColor(this.theme.bg_page)
            .fontColor(this.theme.text_primary)
            .onClick(() => {
              if (this.speakingId === TEST_ID) {
                Speak.stop();
              } else {
                this.test();
              }
            })
        }
      }
      .width('100%')
      // Loaded, but the reference voice failed: said under the line that says loaded.
      if (this.status.loaded && this.status.note.length > 0) {
        Text(this.status.note)
          .fontSize(11)
          .fontColor(this.theme.danger)
          .width('100%')
      }
      if (this.note.length > 0) {
        Text(this.note)
          .fontSize(11)
          .fontColor(this.noteDanger ? this.theme.danger : this.theme.success)
          .width('100%')
      }
      Text($r('app.string.settings_tts_phone_note'))
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')

      Divider().color(this.theme.divider)

      Row({ space: 8 }) {
        Text($r('app.string.settings_reader_enabled'))
          .fontSize(15)
          .fontColor(this.theme.text_primary)
          .layoutWeight(1)
        Toggle({ type: ToggleType.Switch, isOn: this.settings.enabled })
          .onChange((on: boolean) => { this.setEnabled(on); })
      }
      .width('100%')
      Text($r('app.string.settings_reader_enabled_note'))
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')

      Divider().color(this.theme.divider)

      Text($r('app.string.settings_reader_min'))
        .fontSize(13)
        .fontColor(this.theme.text_primary)
        .width('100%')
      TextInput({ text: this.minChars })
        .type(InputType.Number)
        .backgroundColor(this.theme.bg_page)
        .onChange((v: string) => { this.minChars = v; })
        .onSubmit(() => { this.saveNumbers(); })
        .onBlur(() => { this.saveNumbers(); })
      Text($r('app.string.settings_reader_min_note'))
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')

      Text($r('app.string.settings_tts_seed'))
        .fontSize(13)
        .fontColor(this.theme.text_primary)
        .width('100%')
      TextInput({ text: this.seed })
        .backgroundColor(this.theme.bg_page)
        .onChange((v: string) => { this.seed = v; })
        .onSubmit(() => { this.saveNumbers(); })
        .onBlur(() => { this.saveNumbers(); })
      Text($r('app.string.settings_tts_seed_note'))
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')

      Text($r('app.string.settings_tts_temperature'))
        .fontSize(13)
        .fontColor(this.theme.text_primary)
        .width('100%')
      // CustomVoice always reads at the engine's 0.9 (Reader.applySettings).
      TextInput({ text: this.settings.voiceMode === 'custom' ? '0.9' : this.temperature })
        .enabled(this.settings.voiceMode !== 'custom')
        .backgroundColor(this.theme.bg_page)
        .onChange((v: string) => {
          if (this.settings.voiceMode !== 'custom') {
            this.temperature = v;
          }
        })
        .onSubmit(() => { this.saveNumbers(); })
        .onBlur(() => { this.saveNumbers(); })
      Text(this.settings.voiceMode === 'custom'
        ? $r('app.string.settings_tts_temperature_custom_note')
        : $r('app.string.settings_tts_temperature_note', DEFAULT_TEMPERATURE.toString()))
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding(16)
    .margin({ left: 12, right: 12, top: 12, bottom: 24 })
    .borderRadius(12)
    .backgroundColor(this.theme.bg_surface)
  }

  build() {
    Column() {
      Row({ space: 8 }) {
        Button() {
          icon('chevron-left', ICON_TOOLBAR, this.theme.text_secondary)
        }
        .type(ButtonType.Circle)
        .backgroundColor(Color.Transparent)
        .width(40)
        .height(40)
        .onClick(() => router.back())

        Text($r('app.string.settings_page_speech'))
          .fontSize(18)
          .fontWeight(FontWeight.Medium)
          .fontColor(this.theme.text_primary)
          .layoutWeight(1)

        // This page's test, a recorded take, or a reply from the chat still
        // being read: the composer that offers 停止 is not on this page.
        if (this.speaking) {
          Button($r('app.string.settings_stop_playback'))
            .height(32)
            .fontSize(13)
            .backgroundColor(this.theme.bg_surface)
            .fontColor(this.theme.text_primary)
            .onClick(() => {
              Speak.stop();
              player.stop();
            })
        }
      }
      .width('100%')
      .height(56)
      .padding({ left: 8, right: 16 })

      Scroll() {
        Column() {
          Text($r('app.string.settings_tts_intro'))
            .fontSize(12)
            .fontColor(this.theme.text_tertiary)
            .width('100%')
            .padding({ left: 16, right: 16, top: 4, bottom: 8 })

          if (!this.status.available) {
            Text($r('app.string.settings_tts_unavailable'))
              .fontSize(13)
              .fontColor(this.theme.danger)
              .width('100%')
              .padding({ left: 16, right: 16 })
          } else {
            this.sectionTitle($r('app.string.settings_tts_mode'))
            this.modeRows()

            if (this.missing.length > 0 || this.download.running) {
              this.setupCard()
            }
            this.slotsCard()

            if (this.settings.voiceMode === 'custom') {
              this.sectionTitle($r('app.string.settings_tts_speaker'))
              this.speakerCard()
            } else {
              this.sectionTitle($r('app.string.settings_reader_ref'))
              this.referenceCard()
              if (this.recording) {
                Column() {
                  ReferenceRecorder({
                    onUsed: () => {
                      this.recording = false;
                      this.refresh();
                    },
                    onClose: () => {
                      this.recording = false;
                    }
                  })
                }
                .padding({ left: 12, right: 12, top: 12 })
              }
            }

            this.readingCard()
          }
        }
        .width('100%')
      }
      .layoutWeight(1)
      .width('100%')
      .align(Alignment.Top)
    }
    .width('100%')
    .height('100%')
    .backgroundColor(this.theme.bg_page)
  }
}
