/**
 * The third voice, and the number that decides between it and the second one.
 * Reached by typing /tts.
 *
 * Three pages now, and they are not versions of each other:
 *
 *   /asr   SenseVoice plus the prosody profile — what the app will ship.
 *   /omni  MNN plus Qwen2.5-Omni-3B, the model answering in its own voice.
 *   /tts   qwentts.cpp plus Qwen3-TTS, speaking text something ELSE produced.
 *
 * The split matters because /omni's answer was no. It measured 2.8× real time
 * with the diffusion sampler already at its coarsest, and that is paid on top of
 * thinking: 1.4 seconds of silence before a word, then speech slower than
 * speech. This lane takes the text from llama.cpp, which is measured and fast,
 * and only speaks it. On a desktop the same sentences came out at 1.16×.
 *
 * **The voice is written, not cloned.** The field below takes an English
 * description and the same description gives the same speaker across different
 * passages. That sentence cost five attempts: the cloning checkpoint run free
 * changed speaker between texts, including gender; pinned through its speaker
 * encoder it held the speaker and flattened the prosody; in-context cloning
 * fixed both and cost 70% more. design/voice.md has the table.
 *
 * So this page is two measurements at once, and the second one has no number.
 * The ratio is on screen. Whether the voice is worth having is in the ear, and
 * the Play button is there for that.
 */
import { common } from '@kit.AbilityKit';
import { audio } from '@kit.AudioKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { picker } from '@kit.CoreFileKit';
import { router } from '@kit.ArkUI';
import fileUri from '@ohos.file.fileuri';
import bodyclock from 'libbodyclock.so';
import { ICON_TOOLBAR, icon } from '../core/Icons';
import { LocalModelStore, PickedFile } from '../core/LocalModelStore';
import { DEFAULT_FAMILY, Palette, resolvePalette } from '../core/Palettes';
import { TtsModelPair, TtsModelRef, TtsModelStore } from '../core/TtsModelStore';

/** What bodyclock_tts_say returns (src/bodyclock.h). */
interface TtsJson {
  speechSeconds: number;
  seconds: number;
  realtime: number;
  firstAudioSeconds: number;
  chunks: number;
  streamed: boolean;
}

/** The codec's rate, which is NOT the 16 kHz of the input side. */
const SPEECH_SAMPLE_RATE: number = 24000;

/**
 * The description that was chosen on the desktop, and the wording is the point.
 *
 * `gentle, speaks slowly` produced something audibly listless — the words land,
 * so a description that asks for low energy gets low energy. This one asks for
 * the same calm without asking for less of the speaker.
 */
const DEFAULT_VOICE: string = 'breathy, unhurried but not slow';

/**
 * The default, and it is default because it appears in NO reference.
 *
 * The two measurement texts below are the passage `long_zh.wav` was generated
 * from and its first sentence, which makes them the worst possible things to
 * synthesize while cloning that recording: the model would be reciting what it
 * had just been given, and a voice that transfers and a voice that is being
 * echoed would look the same. Judging timbre needs words the reference never
 * contained.
 *
 * Same register and roughly the same length as the short one, so it is quick to
 * judge — but it is NOT interchangeable with it for timing, because it is a
 * different number of frames.
 */
const FRESH_TEXT: string = '先把灯调暗一点吧。今天就到这里，明天早上醒了我们再聊。';

/** Short enough to judge quickly, long enough to have prosody in it. */
const SHORT_TEXT: string = '今天过得怎么样？这是我现在说话的声音。';

/**
 * The passage the desktop's 22-second figure was taken on, and the short line
 * above is its first sentence — so the two differ in length and in nothing else.
 *
 * It is here because the headline ratio is length-dependent in a way that has
 * nothing to do with the engine: prefill is a fixed ~0.86 s per utterance, so
 * RTF is (per-frame cost) + 0.86 / seconds-of-audio. At 3.84 s that tail is
 * 0.22×; at 22 s it is 0.04×. The phone's 2.29 and the desktop's 1.16 were
 * measured at opposite ends of that curve, which flatters the desktop by about
 * a fifth of the gap.
 *
 * Running this passage is what makes the two comparable. It should land near
 * 2.12; noticeably above that would mean the talker's growing KV costs more
 * than the amortisation saves, which is the more interesting outcome.
 */
const LONG_TEXT: string =
  '今天过得怎么样？这是我现在说话的声音。我注意到你最近几天说话的速度比平时慢一些，' +
  '停顿也变多了，这通常和睡眠有关。如果你愿意的话，可以在睡前试着把手机放远一点，' +
  '看看明天早上录音的时候会不会有不一样的感觉。';

/**
 * The seed the pin uses. Any fixed number would do; this one was used on the
 * desktop, so a run here is comparable to what is written down there.
 */
const PINNED_SEED: number = 23;

@Entry
@Component
struct TtsDebug {
  @StorageLink('paletteFamily') @Watch('resolveTheme') paletteFamily: string = DEFAULT_FAMILY;
  @StorageProp('isDark') @Watch('resolveTheme') isDark: boolean = false;
  @State private theme: Palette = resolvePalette(DEFAULT_FAMILY, false);

  @State private engine: boolean = false;
  @State private busy: string = '';
  @State private note: string = '';

  @State private talkerName: string = '';
  @State private codecName: string = '';
  @State private modelType: string = '';
  /** Read from the checkpoint at load; empty for anything but custom_voice. */
  @State private speakers: string[] = [];
  @State private speaker: string = '';

  /** The cloning reference, for a base checkpoint. Seconds held, 0 for none. */
  @State private refName: string = '';
  @State private refSeconds: number = 0;
  /**
   * Pre-filled with the transcript of `long_zh.wav`, which is the reference
   * this lane was built to try first — a hundred Chinese characters is not
   * something to type on a phone.
   *
   * **Wrong for any other recording**, and visibly so: the field is on screen
   * and editable, and a transcript that does not match its audio is the one
   * mistake in-context cloning cannot recover from.
   */
  @State private refText: string = LONG_TEXT;

  @State private voice: string = DEFAULT_VOICE;
  @State private text: string = FRESH_TEXT;
  /** 0 asks the engine to keep its own choice, which on this phone was six. */
  @State private threads: number = 0;
  @State private streaming: boolean = true;
  /**
   * On by default, which is NOT the engine's default.
   *
   * A free-running seed makes every run a different utterance of a different
   * length, and the thread sweep showed what that costs: the frame counts came
   * out at 40, 48, 61, 52, 52, so only the per-frame rates could be compared and
   * the totals could not. Streaming against buffered is a question about the
   * codec decoding the SAME frames two ways, and the pin is what makes it one.
   */
  @State private pinSeed: boolean = true;

  @State private result: TtsJson | undefined = undefined;
  @State private speechBytes: number = 0;

  private talker: TtsModelRef | undefined = undefined;
  private codec: TtsModelRef | undefined = undefined;

  private resolveTheme(): void {
    this.theme = resolvePalette(this.paletteFamily, this.isDark);
  }

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  aboutToAppear(): void {
    this.resolveTheme();
    this.engine = bodyclock.nativeTtsAvailable();
    if (bodyclock.nativeTtsLoaded()) {
      this.modelType = bodyclock.nativeTtsModelType();
    }
    this.restore();
  }

  //----------------------------------------------------------------------------
  // Models
  //----------------------------------------------------------------------------

  /**
   * Re-arm the grants from a previous session and show what was remembered.
   *
   * Two files means two grants, and a persisted grant is dormant until
   * activated: without this the paths open in the session that picked them and
   * fail on every launch after, with both files still sitting there.
   *
   * A half that no longer resolves is forgotten rather than kept. A reference
   * that fails silently every launch is worse than no reference.
   */
  private async restore(): Promise<void> {
    const pair: TtsModelPair = await TtsModelStore.get(this.ctx());
    const talker: TtsModelRef | undefined = pair.talker;
    const codec: TtsModelRef | undefined = pair.codec;

    if (talker !== undefined && await LocalModelStore.activateOne(talker.uri, talker.path)) {
      this.talker = talker;
      this.talkerName = talker.name;
    }
    if (codec !== undefined && await LocalModelStore.activateOne(codec.uri, codec.path)) {
      this.codec = codec;
      this.codecName = codec.name;
    }
  }

  private async pickTalker(): Promise<void> {
    const picked: PickedFile | undefined = await LocalModelStore.pickGguf(this.ctx());
    if (picked === undefined) {
      return;
    }
    const ref: TtsModelRef = { uri: picked.uri, path: picked.path, name: picked.name };
    this.talker = ref;
    this.talkerName = ref.name;
    this.note = '';
    await TtsModelStore.setTalker(this.ctx(), ref);
  }

  private async pickCodec(): Promise<void> {
    const picked: PickedFile | undefined = await LocalModelStore.pickGguf(this.ctx());
    if (picked === undefined) {
      return;
    }
    const ref: TtsModelRef = { uri: picked.uri, path: picked.path, name: picked.name };
    this.codec = ref;
    this.codecName = ref.name;
    this.note = '';
    await TtsModelStore.setCodec(this.ctx(), ref);
  }

  private load(): void {
    const talker: TtsModelRef | undefined = this.talker;
    const codec: TtsModelRef | undefined = this.codec;
    if (talker === undefined || codec === undefined) {
      this.note = 'both halves are needed — a talker and a 12 Hz tokenizer';
      return;
    }

    this.busy = 'loading 2.2 GB…';
    this.note = '';
    bodyclock.nativeTtsLoad(talker.path, codec.path, (type: string, content: string) => {
      this.busy = '';
      if (type === 'ok') {
        this.modelType = content;
        this.readSpeakers();
        // Said here rather than at synthesis time, because the checkpoint
        // decides which parameters are legal. Picking the wrong file is a file
        // name mistake, not a broken model, and this is where it is cheapest to
        // notice.
        this.note = content === 'base' && this.refSeconds === 0
          ? 'a "base" checkpoint clones — pick a reference recording and type ' +
            'what it says, or it has no voice to copy'
          : '';
      } else {
        this.modelType = '';
        this.speakers = [];
        this.speaker = '';
        this.note = content;
      }
    });
  }

  /**
   * The checkpoint's own speaker table, after a load.
   *
   * Asked of the model rather than hard-coded, because "nine named speakers" is
   * something this project read on a model card and never verified — and a
   * count that came out of the file cannot be stale. The first one is selected
   * so a custom_voice model is runnable without a second tap.
   */
  private readSpeakers(): void {
    const raw: string = bodyclock.nativeTtsSpeakers();
    let names: string[] = [];
    try {
      names = JSON.parse(raw) as string[];
    } catch (err) {
      names = [];
    }
    this.speakers = names;
    this.speaker = names.length > 0 ? names[0] : '';
    bodyclock.nativeTtsSetSpeaker(this.speaker);
  }

  /**
   * Pick the recording to clone, and hand it to the core with its transcript.
   *
   * Both go together or neither does: audio alone is the speaker-encoder path,
   * which held identity and flattened prosody, and it is not what this page is
   * for. So the transcript is typed first and the pick is what commits them.
   */
  private async pickReference(): Promise<void> {
    if (this.refText.trim() === '') {
      this.note = 'type what the recording says first — the transcript is what ' +
        'makes this in-context cloning';
      return;
    }

    let uris: string[] = [];
    try {
      const opts: picker.DocumentSelectOptions = new picker.DocumentSelectOptions();
      opts.maxSelectNumber = 1;
      opts.fileSuffixFilters = ['.wav'];
      uris = await new picker.DocumentViewPicker(this.ctx()).select(opts);
    } catch (err) {
      const e = err as BusinessError;
      this.note = 'picker failed: ' + e.code + ' ' + e.message;
      return;
    }
    if (uris.length === 0) {
      return;
    }

    const parsed: fileUri.FileUri = new fileUri.FileUri(uris[0]);
    const rc: number = bodyclock.nativeTtsSetReference(parsed.path, this.refText.trim());
    if (rc !== 0) {
      this.refName = '';
      this.refSeconds = 0;
      this.note = bodyclock.nativeTtsLastError();
      return;
    }
    this.refName = parsed.name;
    this.refSeconds = bodyclock.nativeTtsReferenceSeconds();
    this.result = undefined;
    this.note = '';
  }

  /**
   * Change the thread count, and reload if something is loaded.
   *
   * The reload is not sloppiness, it is the sweep: the engine consumes the
   * count once when it builds its backend, so there is no way to change it in
   * place. After the first load the files are in page cache, and what is being
   * compared is generation, which the reload does not touch.
   */
  private setThreads(n: number): void {
    bodyclock.nativeTtsSetThreads(n);
    this.threads = n;
    this.result = undefined;
    if (this.modelType !== '') {
      this.load();
    }
  }

  //----------------------------------------------------------------------------
  // Speaking
  //----------------------------------------------------------------------------

  private say(): void {
    if (!bodyclock.nativeTtsLoaded()) {
      this.note = 'no model loaded';
      return;
    }
    const line: string = this.text.trim();
    if (line === '') {
      this.note = 'nothing to say';
      return;
    }

    bodyclock.nativeTtsSetVoice(this.voice.trim());
    bodyclock.nativeTtsSetSpeaker(this.speaker);
    bodyclock.nativeTtsSetStreaming(this.streaming);
    bodyclock.nativeTtsSetSeed(this.pinSeed ? PINNED_SEED : -1);
    this.busy = 'speaking…';
    this.note = '';
    this.result = undefined;
    this.speechBytes = 0;

    bodyclock.nativeTtsSay(line, true, (type: string, content: string) => {
      this.busy = '';
      if (type !== 'result') {
        this.note = content;
        return;
      }
      this.result = JSON.parse(content) as TtsJson;
      this.speechBytes = bodyclock.nativeTtsLastSpeech().byteLength;
    });
  }

  /** Play what it said, at ITS rate rather than the recorder's. */
  private async play(): Promise<void> {
    const pcm: ArrayBuffer = bodyclock.nativeTtsLastSpeech();
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

  private result2(): TtsJson {
    return this.result as TtsJson;
  }

  private shortName(name: string): string {
    return name === '' ? '(none)' : name.replace('.gguf', '');
  }

  private statusLine(): string {
    if (this.busy !== '') { return this.busy; }
    if (!this.engine)     { return 'built without qwentts.cpp'; }
    if (this.modelType === '') {
      return this.talkerName === '' || this.codecName === ''
        ? 'pick both halves — the talker and the 12 Hz tokenizer'
        : 'both picked — load them';
    }
    return this.modelType + ' loaded · 24 kHz out';
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

      Text('Speech')
        .fontSize(20)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.theme.text_primary)
        .layoutWeight(1)
      Text(this.engine ? 'qwentts on' : 'no qwentts')
        .fontSize(12)
        .fontColor(this.theme.text_tertiary)
    }
    .width('100%')
    .padding({ left: 16, right: 16, top: 12, bottom: 12 })
  }

  @Builder
  controls() {
    Column({ space: 10 }) {
      // Two files, named on their own buttons, because the commonest failure on
      // this lane is picking the same kind twice.
      Row({ space: 10 }) {
        Button('Talker: ' + this.shortName(this.talkerName))
          .layoutWeight(1)
          .height(40)
          .fontSize(12)
          .backgroundColor(this.theme.bg_surface)
          .fontColor(this.theme.text_primary)
          .enabled(this.engine && this.busy === '')
          .onClick(() => { this.pickTalker(); })

        Button('Codec: ' + this.shortName(this.codecName))
          .layoutWeight(1)
          .height(40)
          .fontSize(12)
          .backgroundColor(this.theme.bg_surface)
          .fontColor(this.theme.text_primary)
          .enabled(this.engine && this.busy === '')
          .onClick(() => { this.pickCodec(); })
      }
      .width('100%')

      Button(this.modelType === '' ? 'Load' : 'Reload')
        .width('100%')
        .height(40)
        .fontSize(14)
        .backgroundColor(this.theme.bg_surface)
        .fontColor(this.theme.text_primary)
        .enabled(this.engine && this.busy === '' &&
                 this.talkerName !== '' && this.codecName !== '')
        .onClick(() => { this.load(); })

      // The thread sweep, and the one control here that costs a reload.
      //
      // **Already swept, and it found nothing**: 4, 6 and 8 are within ~2%,
      // which is inside the ~9% five runs drift from heat, and only 2 is clearly
      // worse. The row stays because a different device may answer differently,
      // and because the drift is itself worth seeing — run the same count twice,
      // ten minutes apart, and the second one is slower.
      //
      // `auto` is the engine's own number, six on this phone, kept on the row so
      // a comparison includes it rather than assuming it.
      Row({ space: 6 }) {
        Text('threads')
          .fontSize(12)
          .fontColor(this.theme.text_secondary)
        ForEach([0, 2, 4, 6, 8], (n: number) => {
          Button(n === 0 ? 'auto' : n.toString())
            .height(32)
            .layoutWeight(1)
            .fontSize(13)
            .backgroundColor(this.threads === n ? this.theme.brand : this.theme.bg_surface)
            .fontColor(this.threads === n ? this.theme.on_brand : this.theme.text_primary)
            .enabled(this.engine && this.busy === '')
            .onClick(() => { this.setThreads(n); })
        }, (n: number) => n.toString())
      }
      .width('100%')

      // Streaming was compared against buffered here and costs nothing — seven
      // alternating runs on a pinned seed, buffered's best codec pass 3073 ms
      // against streaming's 3013. On is the answer; the switch stays because it
      // is how the codec stage gets held still.
      Row({ space: 8 }) {
        Toggle({ type: ToggleType.Switch, isOn: this.streaming })
          .enabled(this.busy === '')
          .onChange((on: boolean) => { this.streaming = on; this.result = undefined; })
        Text('stream — first audio at ~1 s instead of at the end, for free')
          .fontSize(12)
          .fontColor(this.theme.text_secondary)
          .layoutWeight(1)
      }
      .width('100%')

      // The other half of that measurement, and the one to leave alone. Without
      // it every run is a different utterance of a different length, and the
      // codec stage varies ±14% even WITH it — three times the difference the
      // streaming comparison was looking for.
      Row({ space: 8 }) {
        Toggle({ type: ToggleType.Switch, isOn: this.pinSeed })
          .enabled(this.busy === '')
          .onChange((on: boolean) => { this.pinSeed = on; })
        Text('pin the seed — same frames every run, or nothing is comparable')
          .fontSize(12)
          .fontColor(this.theme.text_secondary)
          .layoutWeight(1)
      }
      .width('100%')

      // Only a custom_voice checkpoint has these, and on that one they are not
      // an alternative to the description below — the speaker fixes who is
      // talking, the description still fixes how. That combination is the thing
      // worth hearing here, because it is a stronger guarantee of identity than
      // a description holding steady across passages, with the same control on
      // top. Wrapped, because the count is the model's and not ours.
      if (this.speakers.length > 0) {
        Flex({ wrap: FlexWrap.Wrap }) {
          ForEach(this.speakers, (name: string) => {
            Button(name)
              .height(30)
              .fontSize(12)
              .margin({ right: 6, bottom: 6 })
              .backgroundColor(this.speaker === name ? this.theme.brand : this.theme.bg_surface)
              .fontColor(this.speaker === name ? this.theme.on_brand : this.theme.text_primary)
              .enabled(this.busy === '')
              .onClick(() => { this.speaker = name; this.result = undefined; })
          }, (name: string) => name)
        }
        .width('100%')
      }

      // A base checkpoint clones and takes nothing else: no description, no
      // named speaker. The transcript is not optional — audio alone selects the
      // speaker-encoder path, which held identity and flattened prosody, and
      // in-context cloning is the configuration worth hearing.
      if (this.modelType === 'base') {
        TextInput({ text: this.refText, placeholder: 'what the recording says' })
          .width('100%')
          .height(40)
          .fontSize(13)
          .backgroundColor(this.theme.bg_surface)
          .fontColor(this.theme.text_primary)
          .enabled(this.busy === '')
          .onChange((v: string) => { this.refText = v; })

        Button(this.refSeconds > 0
          ? 'Reference: ' + this.shortName(this.refName) + ' · ' +
            this.fixed(this.refSeconds, 1) + ' s in context every frame'
          : 'Pick a reference .wav — mono 16-bit at 24 kHz')
          .width('100%')
          .height(40)
          .fontSize(12)
          .backgroundColor(this.theme.bg_surface)
          .fontColor(this.theme.text_primary)
          .enabled(this.busy === '')
          .onClick(() => { this.pickReference(); })
      }

      // The control surface, and it really is one: pace, energy, microphone
      // distance and breath are all writable here. Editable rather than a preset
      // list for exactly that reason. A base checkpoint refuses it, so it is not
      // offered there.
      if (this.modelType !== 'base') {
        TextInput({ text: this.voice, placeholder: 'how the voice should sound' })
          .width('100%')
          .height(40)
          .fontSize(13)
          .backgroundColor(this.theme.bg_surface)
          .fontColor(this.theme.text_primary)
          .enabled(this.busy === '')
          .onChange((v: string) => { this.voice = v; })
      }

      TextArea({ text: this.text, placeholder: 'what to say' })
        .width('100%')
        .height(96)
        .fontSize(13)
        .backgroundColor(this.theme.bg_surface)
        .fontColor(this.theme.text_primary)
        .enabled(this.busy === '')
        .onChange((v: string) => { this.text = v; })

      // Three presets, and they are not three lengths of one thing.
      //
      // `short` and `long` are a measurement pair: the same passage and its
      // first sentence, so the only difference between them is duration. That
      // matters because RTF carries prefill / seconds-of-audio on top of the
      // per-frame cost, and comparing two lengths of the same words is how that
      // term was separated from the engine.
      //
      // `fresh` is the default and is in NEITHER — which is the whole point
      // while cloning, since `long` is the transcript of the reference
      // recording and synthesizing it would have the model reciting what it was
      // just given. Still editable; these only save typing on a phone.
      Row({ space: 6 }) {
        Text('text')
          .fontSize(12)
          .fontColor(this.theme.text_secondary)
        Button('fresh')
          .height(32)
          .layoutWeight(1)
          .fontSize(12)
          .backgroundColor(this.text === FRESH_TEXT ? this.theme.brand : this.theme.bg_surface)
          .fontColor(this.text === FRESH_TEXT ? this.theme.on_brand : this.theme.text_primary)
          .enabled(this.busy === '')
          .onClick(() => { this.text = FRESH_TEXT; this.result = undefined; })
        Button('short')
          .height(32)
          .layoutWeight(1)
          .fontSize(12)
          .backgroundColor(this.text === SHORT_TEXT ? this.theme.brand : this.theme.bg_surface)
          .fontColor(this.text === SHORT_TEXT ? this.theme.on_brand : this.theme.text_primary)
          .enabled(this.busy === '')
          .onClick(() => { this.text = SHORT_TEXT; this.result = undefined; })
        Button('long · 22 s')
          .height(32)
          .layoutWeight(1)
          .fontSize(12)
          .backgroundColor(this.text === LONG_TEXT ? this.theme.brand : this.theme.bg_surface)
          .fontColor(this.text === LONG_TEXT ? this.theme.on_brand : this.theme.text_primary)
          .enabled(this.busy === '')
          .onClick(() => { this.text = LONG_TEXT; this.result = undefined; })
      }
      .width('100%')

      // A base checkpoint with no reference has no voice to copy, and the
      // engine's refusal would be correct but late. Disabled is clearer than a
      // run that fails.
      Button('Speak')
        .width('100%')
        .height(44)
        .backgroundColor(this.theme.brand)
        .fontColor(this.theme.on_brand)
        .enabled(this.engine && this.busy === '' && this.modelType !== '' &&
                 (this.modelType !== 'base' || this.refSeconds > 0))
        .onClick(() => { this.say(); })

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

  @Builder
  resultPanel() {
    Column({ space: 10 }) {
      // The number this page exists for, on its own line and named plainly. The
      // same shape /omni uses, so the two are read against each other without
      // having to convert anything.
      Column({ space: 4 }) {
        Text(this.fixed(this.result2().realtime, 2) + '× real time')
          .fontSize(22)
          .fontWeight(FontWeight.Medium)
          .fontColor(this.result2().realtime > 1 ? this.theme.danger : this.theme.success)
        Text('seconds of computation per second of speech. Above 1 it cannot ' +
             'speak as fast as it talks.')
          .fontSize(11)
          .fontColor(this.theme.text_tertiary)
      }
      .alignItems(HorizontalAlign.Start)
      .width('100%')

      Text('spoke ' + this.fixed(this.result2().speechSeconds, 1) + ' s in ' +
           this.fixed(this.result2().seconds, 1) + ' s · ' +
           (this.speechBytes / 2 / SPEECH_SAMPLE_RATE).toFixed(1) + ' s buffered')
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')

      // Below 1× the gaps close on their own and this is the whole remaining
      // wait, which is why it gets a line of its own rather than a footnote.
      // Buffered has no such wait to report: the first sample and the last
      // arrive together, at the end.
      Text(this.result2().streamed
        ? 'streamed · first audio at ' +
          this.fixed(this.result2().firstAudioSeconds * 1000, 0) + ' ms, in ' +
          this.result2().chunks + ' chunks'
        : 'buffered · one pass, nothing arrives before the end')
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .width('100%')

      Button('Play it')
        .width('100%')
        .height(40)
        .backgroundColor(this.theme.bg_surface)
        .fontColor(this.theme.text_primary)
        .onClick(() => { this.play(); })

      // The other half of this page, and it has no number. Whether the voice is
      // worth having is judged by ear, against the platform's own — which
      // design/voice.md measured to be one broadcast timbre on both of its APIs,
      // with no setting that changes it.
      Text('The ratio is one of two answers. The other is whether this sounds ' +
           'better than the platform voice, which is a listening test and not a ' +
           'measurement — play it and decide.')
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

      Scroll() {
        Column() {
          this.controls()

          if (this.result !== undefined) {
            this.resultPanel()
          } else if (this.busy === '') {
            Text('Describe a voice, give it a line, and it speaks. The figure ' +
                 'that matters is how long the speaking takes against how long ' +
                 'it lasts — /omni measured 2.8× at its coarsest setting.')
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
