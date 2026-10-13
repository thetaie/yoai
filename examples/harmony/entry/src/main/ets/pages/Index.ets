/**
 * The chat screen.
 *
 * Bubbles, a composer, a model picker, a history drawer -- and, when a voice is
 * loaded, each reply read aloud as it is written. examples/desktop's App.tsx is
 * a port of this page, including the turn lifecycle, which is the part worth
 * keeping identical: it is where the streamed pieces become one saved message
 * and where "the model said nothing" is told apart from "the user stopped it".
 *
 * The in-flight reply is NOT a member of `messages`. It lives in its own two
 * @State strings and is appended as a message only once the turn ends. Mutating
 * a field inside an object in a @State array does not re-render (only the array
 * itself is observed), so streaming into one would draw nothing until the turn
 * finished -- and working around that by reassigning the whole array per token
 * would rebuild every bubble on screen for each delta.
 */
import { common } from '@kit.AbilityKit';
import { promptAction, router } from '@kit.ArkUI';
import { pasteboard } from '@kit.BasicServicesKit';
import hilog from '@ohos.hilog';
import yoai from 'libyoai.so';
import { RichMessage } from '../components/RichMessage';
import { Canceler, ChatMessage, MessagePart, StreamCallbacks } from '../model/Chat';
import { ModelConfig, modelLabel } from '../model/Provider';
import { ChatEngine } from '../core/ChatEngine';
import { ChatSettings } from '../core/ChatSettings';
import { ICON_INLINE, ICON_TOOLBAR, icon } from '../core/Icons';
import { ModelStore } from '../core/ModelStore';
import { DEFAULT_FAMILY, Palette, resolvePalette } from '../core/Palettes';
import { READER_REV, Reader } from '../core/Reader';
import { SessionMeta, SessionStore } from '../core/SessionStore';
import { Speak, SPEAKING_ID } from '../core/Speak';
import { player, SPEECH_ACTIVE } from '../core/SpeechPlayer';
import { SpeechStore } from '../core/SpeechStore';

/** The `usage` event's payload, once parsed. Every field may be absent. */
interface Usage {
  model?: string;
  inputTokens?: number;
  outputTokens?: number;
  thoughtTokens?: number;
  totalTokens?: number;
  totalCost?: number;
}

@Entry
@Component
struct Index {
  @State private messages: ChatMessage[] = [];
  @State private input: string = '';

  /** The turn in flight, if any. */
  @State private streaming: boolean = false;
  @State private streamText: string = '';
  @State private streamThinking: string = '';
  /** The last finished turn's token summary, shown on the message it belongs to. */
  @State private usage: Usage | null = null;

  @State private models: ModelConfig[] = [];
  @State private activeId: string = '';
  @State private sessions: SessionMeta[] = [];
  @State private showDrawer: boolean = false;

  /**
   * Whether there is speech to stop, from a reply or a message's 朗读. It
   * outlasts the turn -- the engine writes faster than the speaker plays -- so
   * the composer offers 停止 until it ends.
   */
  @StorageProp(SPEECH_ACTIVE) speaking: boolean = false;
  /** Which message's 朗读 is speaking, '' for none. */
  @StorageProp(SPEAKING_ID) speakingId: string = '';
  /** Bumped when the voice loads or goes, so 朗读 appears and disappears with it. */
  @StorageProp(READER_REV) readerRev: number = 0;

  @StorageLink('fontScale') fontScale: number = 1;

  /**
   * The palette in force, as one object the build method reads fields off.
   *
   * It is @State recomputed by a watcher rather than a method called at each
   * use site: there are ~36 of those in this file, and resolving the palette
   * per reference would walk the table on every one of them, every frame.
   *
   * `paletteFamily` is the user's choice; `isDark` is what the system settled
   * on, published by EntryAbility. Neither means anything without the other.
   */
  @StorageLink('paletteFamily') @Watch('resolveTheme') paletteFamily: string = DEFAULT_FAMILY;
  @StorageProp('isDark') @Watch('resolveTheme') isDark: boolean = false;
  @State private theme: Palette = resolvePalette(DEFAULT_FAMILY, false);

  private resolveTheme(): void {
    this.theme = resolvePalette(this.paletteFamily, this.isDark);
  }

  private sessionId: string = SessionStore.newId();
  private canceler: Canceler | null = null;
  /**
   * Stop was pressed while the turn was still being set up -- the voice opening,
   * the prompt being built -- before it had anything to cancel.
   */
  private stopAsked: boolean = false;
  private scroller: Scroller = new Scroller();
  /** Set once startup has re-armed the stored grants; loads wait for it. */
  private started: boolean = false;

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  async aboutToAppear(): Promise<void> {
    // @Watch fires on change, not on construction, and `theme` was initialized
    // with defaults while the two inputs came from AppStorage already set. So
    // the first resolve has to be done by hand or a cold start paints the
    // default palette until something happens to move one of them.
    this.resolveTheme();

    const ctx: common.UIAbilityContext = this.ctx();
    Reader.init(ctx);
    // Whether replies are read changes the prompt they are sent with; whichever
    // of the model's load and the voice's finishes last warms it.
    Reader.onVoiceChanged = () => {
      ModelStore.warmActive(ctx);
    };

    // The speech settings first: they re-arm the download folder's grant, and
    // a model downloaded there has no grant of its own to re-arm.
    await SpeechStore.init(ctx);
    // The native registry is process memory, so a cold start finds it empty.
    // This is the one call that makes stored models usable again.
    await ModelStore.syncToCore(ctx);
    this.started = true;
    await this.reloadModels();
    this.sessions = await SessionStore.index(ctx);

    // Configured IS enabled: the selected on-device model and the voice are
    // read now, while the screen is up, rather than when the first reply needs
    // them -- which is the one moment somebody is already waiting.
    ModelStore.autoPreload(ctx);
    Reader.autoLoadVoice();
  }

  onPageShow(): void {
    // Coming back from Settings, the model list may have changed under us. And
    // coming back from /tts, whose engine shares the voice's weights, the
    // voice may have been unloaded; this loads it again, and does nothing
    // when it is up.
    if (this.started) {
      this.reloadModels();
      Reader.autoLoadVoice();
    }
  }

  private async reloadModels(): Promise<void> {
    this.models = await ModelStore.list(this.ctx());
    const stored: string = await ModelStore.activeId(this.ctx());

    // Only offer what the core will actually run: an entry with no key behind
    // it is absent from yoai_providers(), and picking it could only fail.
    const runnable: string = yoai.nativeProviders();
    const ready: Set<string> = new Set<string>(runnable.length === 0 ? [] : runnable.split('\n'));

    let next: string = ready.has(stored) ? stored : '';
    if (next.length === 0) {
      for (const m of this.models) {
        if (ready.has(m.id)) {
          next = m.id;
          break;
        }
      }
    }
    if (next !== stored) {
      await ModelStore.setActiveId(this.ctx(), next);
    }
    this.activeId = next;
  }

  private activeLabel(): string {
    for (const m of this.models) {
      if (m.id === this.activeId) {
        return modelLabel(m);
      }
    }
    return '';
  }

  private async selectModel(id: string): Promise<void> {
    this.activeId = id;
    await ModelStore.setActiveId(this.ctx(), id);
    // Switching to an on-device model starts reading its weights now rather
    // than on the first message.
    ModelStore.autoPreload(this.ctx());
  }

  //----------------------------------------------------------------------------
  // Sending
  //----------------------------------------------------------------------------

  /**
   * Handle a slash command, and report whether one was handled.
   *
   * Only an exact match counts. A message that merely starts with a slash is a
   * message -- "/etc is where it lives" is a sentence, and swallowing it would
   * be worse than having no commands at all.
   */
  private runCommand(text: string): boolean {
    if (text === '/asr' || text === '/voice') {
      router.pushUrl({ url: 'pages/VoiceDebug' });
      return true;
    }
    // Retired. The natives behind OmniDebug are commented out in napi_init.cpp,
    // because src/voice/omni.cpp moved to src/voice/attic/ and is in no target.
    // The page is still in the tree and still compiles; it just has nothing to
    // call, so the route goes rather than leaving a command that crashes.
    // if (text === '/omni') {
    //   router.pushUrl({ url: 'pages/OmniDebug' });
    //   return true;
    // }
    if (text === '/tts') {
      router.pushUrl({ url: 'pages/TtsDebug' });
      return true;
    }
    return false;
  }

  private async send(): Promise<void> {
    const text: string = this.input.trim();
    if (text.length === 0 || this.streaming) {
      return;
    }

    // Slash commands: typed, not discoverable, and deliberately so. These open
    // developer pages that have no business in the navigation of a shipping app
    // -- someone who needs one knows its name. The check runs BEFORE the
    // no-model guard below, because none of them need a model.
    if (this.runCommand(text)) {
      this.input = '';
      return;
    }

    if (this.activeId.length === 0) {
      this.note($r('app.string.chat_need_model'));
      return;
    }

    this.messages.push({ role: 'user', content: text, ts: Date.now() });
    this.input = '';
    this.streaming = true;
    this.streamText = '';
    this.streamThinking = '';
    this.usage = null;
    this.stopAsked = false;
    this.scrollToEnd();

    const label: string = this.activeLabel();
    const providerId: string = this.activeId;

    // A new reply supersedes the old one out loud as well as on screen.
    await Speak.stop();

    // An on-device model still loading is waited for here rather than left to
    // the turn: the turn would load it straight, and mapped straight from user
    // storage a large model froze this thread until the system killed the app
    // (see nativePrefetch). Immediate for a remote model or one already loaded.
    await ModelStore.autoPreload(this.ctx());

    // The reading lane is decided BEFORE the turn starts, because it has to be
    // open before the first token arrives -- and asked fresh, because the voice
    // loads in the background.
    const stream: number = Reader.ready() ? await Reader.begin() : 0;
    const reading: boolean = stream !== 0;

    // A reply that will be heard is asked to be written for the ear. Built the
    // way the on-device model's warm-up builds it, so the prompt computed ahead
    // of this turn is the one it sends.
    const systemPrompt: string = await ChatSettings.turnPrompt(this.ctx(), reading);

    const cb: StreamCallbacks = {
      onDelta: (t: string) => {
        this.streamText = this.streamText + t;
        // The same delta the transcript gets, markdown and all: the core turns
        // the reply into what should be spoken. Dropped once a message's 朗读
        // has taken the voice.
        if (reading) {
          Reader.feed(stream, t);
        }
        this.scrollToEnd();
      },
      onThinking: (t: string) => {
        this.streamThinking = this.streamThinking + t;
        this.scrollToEnd();
      },
      onUsage: (json: string) => {
        try {
          this.usage = JSON.parse(json) as Usage;
        } catch (err) {
          // A summary that will not parse is worth nothing and costs nothing to
          // drop; the reply itself already arrived.
        }
      },
      onError: (message: string) => {
        this.messages.push({ role: 'assistant', content: message, transient: true, ts: Date.now() });
      },
      onDone: (aborted: boolean) => {
        if (reading) {
          // The stream has to be closed either way -- there are engines and
          // threads behind it. When the user stopped the turn they asked for
          // silence, so it is cancelled rather than read to the end. Either is
          // a no-op when the stream is no longer this reply's.
          if (aborted) {
            Reader.cancel(stream);
          } else {
            Reader.finish(stream).catch((e: Error) => {
              hilog.error(0x0000, 'yoai', 'reading the reply failed: %{public}s', e.message);
            });
          }
        }
        this.finishTurn(label, aborted);
      }
    };
    this.canceler = ChatEngine.run(providerId, this.messages, systemPrompt, cb);
    // The request is out by now, so cancelling is the only way to stop it; the
    // turn still ends through onDone like any other.
    if (this.stopAsked && this.canceler !== null) {
      this.canceler.cancel();
    }
  }

  /**
   * Fold the streamed pieces into one assistant message and persist.
   *
   * A turn that produced nothing at all still gets a row: silence after a send
   * reads as the app being broken, and "the model returned nothing" is at least
   * actionable. An aborted turn keeps whatever arrived -- the user stopped it,
   * they did not ask to lose it.
   */
  private finishTurn(label: string, aborted: boolean): void {
    const parts: MessagePart[] = [];
    if (this.streamThinking.length > 0) {
      parts.push({ kind: 'thinking', content: this.streamThinking });
    }
    if (this.streamText.length > 0) {
      parts.push({ kind: 'text', content: this.streamText });
    }

    if (parts.length > 0) {
      this.messages.push({
        role: 'assistant',
        content: this.streamText,
        parts: parts,
        model: label,
        ts: Date.now()
      });
    } else if (!aborted) {
      const last: ChatMessage | undefined = this.messages[this.messages.length - 1];
      const alreadyReported: boolean = last !== undefined && last.transient === true;
      if (!alreadyReported) {
        this.note($r('app.string.chat_empty_reply'));
      }
    }

    this.streaming = false;
    this.streamText = '';
    this.streamThinking = '';
    this.canceler = null;
    this.persist();
    this.scrollToEnd();
  }

  /**
   * Silence, whichever lane is speaking and whatever state its engine is in: a
   * reply, a message's 朗读, a recorded take.
   */
  private stopSpeech(): void {
    Speak.stop();
    player.stop();
  }

  /** The composer's stop: the turn if one is running, and the speech. */
  private stop(): void {
    if (this.canceler !== null) {
      this.canceler.cancel();
    } else if (this.streaming) {
      this.stopAsked = true;
    }
    this.stopSpeech();
  }

  private async persist(): Promise<void> {
    await SessionStore.save(this.ctx(), this.sessionId, this.messages);
    this.sessions = await SessionStore.index(this.ctx());
  }

  /**
   * Say something to the user in the thread. Transient rows render like a reply
   * but are never sent to a model and never saved.
   *
   * Takes a Resource rather than a string so every such line is translatable --
   * this is the app's own voice, and the one place it would be easy to hardcode
   * English into a Chinese-first UI.
   */
  private note(res: Resource): void {
    this.messages.push({
      role: 'assistant',
      content: this.ctx().resourceManager.getStringSync(res),
      transient: true,
      ts: Date.now()
    });
    this.scrollToEnd();
  }

  private scrollToEnd(): void {
    this.scroller.scrollEdge(Edge.Bottom);
  }

  //----------------------------------------------------------------------------
  // Sessions
  //----------------------------------------------------------------------------

  /**
   * A new conversation, for the engines as well as the screen. Clearing the
   * transcript is enough for a remote provider, which is handed the whole
   * conversation every turn and keeps nothing; an on-device one keeps a KV
   * cache, and the next prompt having one message in it does not empty it.
   */
  private async newChat(): Promise<void> {
    this.stop();
    await this.persist();
    ModelStore.forgetAll(this.ctx());
    this.sessionId = SessionStore.newId();
    this.messages = [];
    this.usage = null;
    this.showDrawer = false;
  }

  private async openSession(id: string): Promise<void> {
    this.stop();
    await this.persist();
    this.sessionId = id;
    this.messages = await SessionStore.messages(this.ctx(), id);
    this.usage = null;
    this.showDrawer = false;
    this.scrollToEnd();
  }

  private async deleteSession(id: string): Promise<void> {
    await SessionStore.remove(this.ctx(), id);
    this.sessions = await SessionStore.index(this.ctx());
    if (id === this.sessionId) {
      ModelStore.forgetAll(this.ctx());
      this.sessionId = SessionStore.newId();
      this.messages = [];
      this.usage = null;
    }
  }

  private copy(text: string): void {
    const data: pasteboard.PasteData = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
    pasteboard.getSystemPasteboard().setData(data).then(() => {
      promptAction.showToast({ message: $r('app.string.toast_copied') });
    });
  }

  //----------------------------------------------------------------------------
  // Speaking a message
  //----------------------------------------------------------------------------

  /**
   * What 朗读 reads: the answer, not the thinking. A model's chain-of-thought
   * is not part of the answer, and reading it aloud is minutes of text nobody
   * asked for -- the same rule that keeps it folded on screen.
   */
  private spokenOf(m: ChatMessage): string {
    if (m.parts === undefined) {
      return m.content;
    }
    const out: string[] = [];
    for (const p of m.parts) {
      if (p.kind !== 'thinking') {
        out.push(p.content);
      }
    }
    return out.join('\n');
  }

  private async speakMessage(id: string, m: ChatMessage): Promise<void> {
    if (this.speakingId === id) {
      Speak.stop();
      return;
    }
    const why: string = await Speak.speak(id, this.spokenOf(m));
    if (why.length > 0) {
      promptAction.showToast({ message: $r('app.string.chat_speak_failed', why) });
    }
  }

  /** "输入 1,024 · 输出 512 tokens" -- absent fields are left out. */
  private usageLine(): string {
    const u: Usage | null = this.usage;
    if (u === null) {
      return '';
    }
    const rm = this.ctx().resourceManager;
    const bits: string[] = [];
    if (u.inputTokens !== undefined && u.outputTokens !== undefined) {
      bits.push(rm.getStringSync($r('app.string.chat_usage').id,
        u.inputTokens.toLocaleString(), u.outputTokens.toLocaleString()));
    }
    if (u.thoughtTokens !== undefined && u.thoughtTokens > 0) {
      bits.push(rm.getStringSync($r('app.string.chat_usage_thinking').id, u.thoughtTokens.toLocaleString()));
    }
    // Only a real cost. An on-device turn reports 0, and saying so on every
    // reply is a line of text to tell the reader nothing happened.
    if (u.totalCost !== undefined && u.totalCost > 0) {
      bits.push('$' + u.totalCost.toFixed(4));
    }
    return bits.join(' · ');
  }

  //----------------------------------------------------------------------------
  // Views
  //----------------------------------------------------------------------------

  @Builder
  modelMenu() {
    Menu() {
      ForEach(this.models, (m: ModelConfig) => {
        MenuItem({ content: modelLabel(m) })
          .onClick(() => {
            this.selectModel(m.id);
          })
      }, (m: ModelConfig) => m.id)
      // Straight to the models page, not the settings index: this item is
      // reached from the model picker and means one specific thing.
      MenuItem({ content: $r('app.string.settings_add') })
        .onClick(() => router.pushUrl({ url: 'pages/SettingsModels' }))
    }
  }

  @Builder
  header() {
    Row({ space: 8 }) {
      Button() {
        icon('menu', ICON_TOOLBAR, this.theme.text_secondary)
      }
      .type(ButtonType.Circle)
      .backgroundColor(Color.Transparent)
      .width(40)
      .height(40)
      .onClick(() => { this.showDrawer = true; })

      Row({ space: 4 }) {
        Text(this.activeId.length === 0 ? $r('app.string.chat_pick_model') : this.activeLabel())
          .fontSize(16)
          .fontWeight(FontWeight.Medium)
          .fontColor(this.theme.text_primary)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
        icon('chevron-down', ICON_INLINE, this.theme.text_tertiary)
      }
      .layoutWeight(1)
      .justifyContent(FlexAlign.Center)
      .bindMenu(this.modelMenu())

      Button() {
        icon('settings', ICON_TOOLBAR, this.theme.text_secondary)
      }
      .type(ButtonType.Circle)
      .backgroundColor(Color.Transparent)
      .width(40)
      .height(40)
      .onClick(() => router.pushUrl({ url: 'pages/Settings' }))
    }
    .width('100%')
    .height(56)
    .padding({ left: 8, right: 8 })
    .backgroundColor(this.theme.bg_page)
  }

  @Builder
  emptyState() {
    Column({ space: 8 }) {
      Text($r('app.string.chat_empty_title'))
        .fontSize(20)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.theme.text_primary)
        .textAlign(TextAlign.Center)
      Text($r('app.string.chat_empty_body'))
        .fontSize(14)
        .fontColor(this.theme.text_tertiary)
        .textAlign(TextAlign.Center)
    }
    .width('100%')
    .padding(32)
  }

  @Builder
  userBubble(m: ChatMessage) {
    Row() {
      Text(m.content)
        .fontSize(16 * this.fontScale)
        .fontColor(this.theme.text_primary)
        .backgroundColor(this.theme.bg_bubble)
        .borderRadius(16)
        .padding({ left: 14, right: 14, top: 10, bottom: 10 })
        .copyOption(CopyOptions.LocalDevice)
    }
    .width('100%')
    .justifyContent(FlexAlign.End)
    .padding({ left: 48, right: 12, top: 6, bottom: 6 })
  }

  /**
   * Whether 朗读 can be offered. Reads readerRev so the buttons follow the
   * voice as it loads; the value itself is not the answer.
   */
  private voiceUp(): boolean {
    return this.readerRev >= 0 && Reader.canSpeak();
  }

  /**
   * `index` rather than a "last" flag: an item's builder is not run again when
   * a message is appended after it, so a flag passed in would stay true on the
   * message that used to be last, while the length read here does not.
   */
  @Builder
  assistantBubble(m: ChatMessage, id: string, index: number) {
    Column({ space: 6 }) {
      if (m.transient === true) {
        // The app's own words -- an error, a notice -- shown as written.
        Text(m.content)
          .fontSize(16 * this.fontScale)
          .fontColor(this.theme.danger)
          .width('100%')
          .copyOption(CopyOptions.LocalDevice)
      } else {
        RichMessage({
          text: this.spokenOf(m),
          thinking: this.thinkingOf(m),
          streaming: false,
          fontScale: this.fontScale
        })
      }

      if (m.model !== undefined) {
        Row({ space: 12 }) {
          Text(m.model).fontSize(11).fontColor(this.theme.text_tertiary)
          if (index === this.messages.length - 1 && !this.streaming && this.usage !== null) {
            Text(this.usageLine())
              .fontSize(11)
              .fontColor(this.theme.text_tertiary)
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
              .layoutWeight(1)
          }
          Text($r('app.string.action_copy'))
            .fontSize(11)
            .fontColor(this.theme.text_tertiary)
            .onClick(() => this.copy(m.content))
          // Offered once a voice is up; a button that can only refuse is worse
          // than none.
          if (this.speakingId === id || this.voiceUp()) {
            Text(this.speakingId === id
              ? $r('app.string.action_stop_speaking')
              : $r('app.string.action_speak'))
              .fontSize(11)
              .fontColor(this.speakingId === id ? this.theme.brand : this.theme.text_tertiary)
              .onClick(() => { this.speakMessage(id, m); })
          }
        }
        .width('100%')
      }
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding({ left: 12, right: 12, top: 6, bottom: 6 })
  }

  private thinkingOf(m: ChatMessage): string {
    if (m.parts === undefined) {
      return '';
    }
    const out: string[] = [];
    for (const p of m.parts) {
      if (p.kind === 'thinking') {
        out.push(p.content);
      }
    }
    return out.join('\n');
  }

  @Builder
  streamingBubble() {
    Column({ space: 6 }) {
      if (this.streamText.length === 0 && this.streamThinking.length === 0) {
        Row() {
          LoadingProgress().width(20).height(20).color(this.theme.text_tertiary)
        }
      } else {
        RichMessage({
          text: this.streamText,
          thinking: this.streamThinking,
          streaming: true,
          fontScale: this.fontScale
        })
      }
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding({ left: 12, right: 12, top: 6, bottom: 6 })
  }

  @Builder
  composer() {
    Column({ space: 6 }) {
      Row({ space: 8 }) {
        TextInput({ text: this.input, placeholder: $r('app.string.chat_input_hint') })
          .layoutWeight(1)
          .backgroundColor(this.theme.bg_surface)
          .borderRadius(22)
          .padding({ left: 16, right: 16 })
          .fontSize(16 * this.fontScale)
          .onChange((v: string) => { this.input = v; })
          .onSubmit(() => this.send())

        // Stop while a turn runs, and for as long as speech is playing after it.
        Button() {
          Text(this.streaming || this.speaking ? '■' : '↑').fontSize(18).fontColor(this.theme.on_brand)
        }
        .type(ButtonType.Circle)
        .width(44)
        .height(44)
        .backgroundColor(this.theme.brand)
        .onClick(() => {
          if (this.streaming || this.speaking) {
            this.stop();
          } else {
            this.send();
          }
        })
      }
      .width('100%')

      Text($r('app.string.chat_disclaimer'))
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
    }
    .width('100%')
    .padding({ left: 12, right: 12, top: 8, bottom: 12 })
    .backgroundColor(this.theme.bg_page)
  }

  @Builder
  drawer() {
    Column() {
      Row() {
        Text($r('app.string.drawer_title'))
          .fontSize(18)
          .fontWeight(FontWeight.Medium)
          .fontColor(this.theme.text_primary)
          .layoutWeight(1)
        Text($r('app.string.drawer_new_chat'))
          .fontSize(14)
          .fontColor(this.theme.brand)
          .onClick(() => this.newChat())
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 16, bottom: 8 })

      if (this.sessions.length === 0) {
        Text($r('app.string.drawer_empty'))
          .fontSize(14)
          .fontColor(this.theme.text_tertiary)
          .padding(16)
      } else {
        List({ space: 2 }) {
          ForEach(this.sessions, (s: SessionMeta) => {
            ListItem() {
              Column({ space: 2 }) {
                Text(s.title.length === 0 ? $r('app.string.drawer_untitled') : s.title)
                  .fontSize(15)
                  .fontColor(this.theme.text_primary)
                  .maxLines(1)
                  .textOverflow({ overflow: TextOverflow.Ellipsis })
                  .width('100%')
                Text($r('app.plural.drawer_msg_count', s.count, s.count))
                  .fontSize(12)
                  .fontColor(this.theme.text_tertiary)
              }
              .alignItems(HorizontalAlign.Start)
              .width('100%')
              .padding({ left: 16, right: 16, top: 10, bottom: 10 })
              .backgroundColor(s.id === this.sessionId ? this.theme.bg_overlay : Color.Transparent)
            }
            .onClick(() => this.openSession(s.id))
            .swipeAction({ end: this.deleteButton(s.id) })
          }, (s: SessionMeta) => s.id + '-' + s.updatedAt.toString())
        }
        .layoutWeight(1)
        .width('100%')
      }

      Text($r('app.string.drawer_local_only'))
        .fontSize(11)
        .fontColor(this.theme.text_tertiary)
        .padding(16)
    }
    .width('100%')
    .height('100%')
    .backgroundColor(this.theme.bg_surface)
  }

  @Builder
  deleteButton(id: string) {
    Button($r('app.string.settings_delete'))
      .fontSize(14)
      .fontColor(this.theme.on_brand)
      .backgroundColor(this.theme.danger)
      .height('100%')
      .onClick(() => this.deleteSession(id))
  }

  build() {
    Column() {
      this.header()
      Divider().color(this.theme.divider)

      List({ space: 0, scroller: this.scroller }) {
        if (this.messages.length === 0 && !this.streaming) {
          ListItem() {
            this.emptyState()
          }
        }
        ForEach(this.messages, (m: ChatMessage, i: number) => {
          ListItem() {
            if (m.role === 'user') {
              this.userBubble(m)
            } else {
              this.assistantBubble(m, 'm' + i, i)
            }
          }
        }, (m: ChatMessage, i: number) => i.toString() + '-' + (m.ts === undefined ? '' : m.ts.toString()))
        if (this.streaming) {
          ListItem() {
            this.streamingBubble()
          }
        }
      }
      .layoutWeight(1)
      .width('100%')
      .edgeEffect(EdgeEffect.Spring)

      this.composer()
    }
    .width('100%')
    .height('100%')
    .backgroundColor(this.theme.bg_page)
    .bindSheet($$this.showDrawer, this.drawer(), {
      detents: [SheetSize.MEDIUM, SheetSize.LARGE],
      backgroundColor: this.theme.bg_surface
    })
  }
}
