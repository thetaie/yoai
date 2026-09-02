/**
 * The chat screen.
 *
 * Deliberately plain: bubbles, a composer, a model picker, a history drawer.
 * Markdown, charts and math rendering are not here yet -- a reply is shown as
 * the text it is. That keeps the one thing this page must get right visible:
 * the streaming loop, from a keystroke through the native core and back a token
 * at a time.
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
import yoai from 'libyoai.so';
import { Canceler, ChatMessage, MessagePart, StreamCallbacks } from '../model/Chat';
import { ModelConfig, modelLabel } from '../model/Provider';
import { ChatEngine } from '../core/ChatEngine';
import { ModelStore } from '../core/ModelStore';
import { SessionMeta, SessionStore } from '../core/SessionStore';

@Entry
@Component
struct Index {
  @State private messages: ChatMessage[] = [];
  @State private input: string = '';

  /** The turn in flight, if any. */
  @State private streaming: boolean = false;
  @State private streamText: string = '';
  @State private streamThinking: string = '';

  @State private models: ModelConfig[] = [];
  @State private activeId: string = '';
  @State private sessions: SessionMeta[] = [];
  @State private showDrawer: boolean = false;

  @StorageLink('fontScale') fontScale: number = 1;

  private sessionId: string = SessionStore.newId();
  private canceler: Canceler | null = null;
  private scroller: Scroller = new Scroller();

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  async aboutToAppear(): Promise<void> {
    // The native registry is process memory, so a cold start finds it empty.
    // This is the one call that makes stored models usable again.
    await ModelStore.syncToCore(this.ctx());
    await this.reloadModels();
    this.sessions = await SessionStore.index(this.ctx());
  }

  onPageShow(): void {
    // Coming back from Settings, the model list may have changed under us.
    this.reloadModels();
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

  //----------------------------------------------------------------------------
  // Sending
  //----------------------------------------------------------------------------

  private async send(): Promise<void> {
    const text: string = this.input.trim();
    if (text.length === 0 || this.streaming) {
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
    this.scrollToEnd();

    const label: string = this.activeLabel();
    const cb: StreamCallbacks = {
      onDelta: (t: string) => {
        this.streamText = this.streamText + t;
        this.scrollToEnd();
      },
      onThinking: (t: string) => {
        this.streamThinking = this.streamThinking + t;
        this.scrollToEnd();
      },
      onError: (message: string) => {
        this.messages.push({ role: 'assistant', content: message, transient: true, ts: Date.now() });
      },
      onDone: (aborted: boolean) => {
        this.finishTurn(label, aborted);
      }
    };
    this.canceler = ChatEngine.run(this.activeId, this.messages, '', cb);
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

  private stop(): void {
    if (this.canceler !== null) {
      this.canceler.cancel();
    }
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

  private async newChat(): Promise<void> {
    this.stop();
    await this.persist();
    this.sessionId = SessionStore.newId();
    this.messages = [];
    this.showDrawer = false;
  }

  private async openSession(id: string): Promise<void> {
    this.stop();
    await this.persist();
    this.sessionId = id;
    this.messages = await SessionStore.messages(this.ctx(), id);
    this.showDrawer = false;
    this.scrollToEnd();
  }

  private async deleteSession(id: string): Promise<void> {
    await SessionStore.remove(this.ctx(), id);
    this.sessions = await SessionStore.index(this.ctx());
    if (id === this.sessionId) {
      this.sessionId = SessionStore.newId();
      this.messages = [];
    }
  }

  private copy(text: string): void {
    const data: pasteboard.PasteData = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
    pasteboard.getSystemPasteboard().setData(data).then(() => {
      promptAction.showToast({ message: $r('app.string.toast_copied') });
    });
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
            this.activeId = m.id;
            ModelStore.setActiveId(this.ctx(), m.id);
          })
      }, (m: ModelConfig) => m.id)
      MenuItem({ content: $r('app.string.settings_add') })
        .onClick(() => router.pushUrl({ url: 'pages/Settings' }))
    }
  }

  @Builder
  header() {
    Row({ space: 8 }) {
      Button() {
        Text('☰').fontSize(20).fontColor($r('app.color.text_secondary'))
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
          .fontColor($r('app.color.text_primary'))
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
        Text('▾').fontSize(12).fontColor($r('app.color.text_tertiary'))
      }
      .layoutWeight(1)
      .justifyContent(FlexAlign.Center)
      .bindMenu(this.modelMenu())

      Button() {
        Text('⚙').fontSize(18).fontColor($r('app.color.text_secondary'))
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
    .backgroundColor($r('app.color.bg_page'))
  }

  @Builder
  emptyState() {
    Column({ space: 8 }) {
      Text($r('app.string.chat_empty_title'))
        .fontSize(20)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .textAlign(TextAlign.Center)
      Text($r('app.string.chat_empty_body'))
        .fontSize(14)
        .fontColor($r('app.color.text_tertiary'))
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
        .fontColor($r('app.color.text_primary'))
        .backgroundColor($r('app.color.bg_bubble'))
        .borderRadius(16)
        .padding({ left: 14, right: 14, top: 10, bottom: 10 })
        .copyOption(CopyOptions.LocalDevice)
    }
    .width('100%')
    .justifyContent(FlexAlign.End)
    .padding({ left: 48, right: 12, top: 6, bottom: 6 })
  }

  @Builder
  thinkingBlock(text: string) {
    Column({ space: 4 }) {
      Text($r('app.string.chat_thinking_label'))
        .fontSize(12)
        .fontColor($r('app.color.text_tertiary'))
      Text(text)
        .fontSize(14 * this.fontScale)
        .fontColor($r('app.color.text_tertiary'))
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding(10)
    .borderRadius(10)
    .backgroundColor($r('app.color.bg_overlay'))
  }

  @Builder
  assistantBubble(m: ChatMessage) {
    Column({ space: 6 }) {
      if (m.parts !== undefined) {
        ForEach(m.parts, (p: MessagePart) => {
          if (p.kind === 'thinking') {
            this.thinkingBlock(p.content)
          } else {
            Text(p.content)
              .fontSize(16 * this.fontScale)
              .fontColor($r('app.color.text_primary'))
              .width('100%')
              .copyOption(CopyOptions.LocalDevice)
          }
        }, (p: MessagePart, i: number) => i.toString())
      } else {
        Text(m.content)
          .fontSize(16 * this.fontScale)
          .fontColor(m.transient === true ? $r('app.color.danger') : $r('app.color.text_primary'))
          .width('100%')
          .copyOption(CopyOptions.LocalDevice)
      }

      if (m.model !== undefined) {
        Row({ space: 12 }) {
          Text(m.model).fontSize(11).fontColor($r('app.color.text_tertiary'))
          Text($r('app.string.action_copy'))
            .fontSize(11)
            .fontColor($r('app.color.text_tertiary'))
            .onClick(() => this.copy(m.content))
        }
        .width('100%')
      }
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding({ left: 12, right: 12, top: 6, bottom: 6 })
  }

  @Builder
  streamingBubble() {
    Column({ space: 6 }) {
      if (this.streamThinking.length > 0) {
        this.thinkingBlock(this.streamThinking)
      }
      if (this.streamText.length > 0) {
        Text(this.streamText)
          .fontSize(16 * this.fontScale)
          .fontColor($r('app.color.text_primary'))
          .width('100%')
      } else if (this.streamThinking.length === 0) {
        Row() {
          LoadingProgress().width(20).height(20).color($r('app.color.text_tertiary'))
        }
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
          .backgroundColor($r('app.color.bg_surface'))
          .borderRadius(22)
          .padding({ left: 16, right: 16 })
          .fontSize(16 * this.fontScale)
          .onChange((v: string) => { this.input = v; })
          .onSubmit(() => this.send())

        Button() {
          Text(this.streaming ? '■' : '↑').fontSize(18).fontColor($r('app.color.on_brand'))
        }
        .type(ButtonType.Circle)
        .width(44)
        .height(44)
        .backgroundColor($r('app.color.brand'))
        .onClick(() => {
          if (this.streaming) {
            this.stop();
          } else {
            this.send();
          }
        })
      }
      .width('100%')

      Text($r('app.string.chat_disclaimer'))
        .fontSize(11)
        .fontColor($r('app.color.text_tertiary'))
    }
    .width('100%')
    .padding({ left: 12, right: 12, top: 8, bottom: 12 })
    .backgroundColor($r('app.color.bg_page'))
  }

  @Builder
  drawer() {
    Column() {
      Row() {
        Text($r('app.string.drawer_title'))
          .fontSize(18)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
          .layoutWeight(1)
        Text($r('app.string.drawer_new_chat'))
          .fontSize(14)
          .fontColor($r('app.color.brand'))
          .onClick(() => this.newChat())
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 16, bottom: 8 })

      if (this.sessions.length === 0) {
        Text($r('app.string.drawer_empty'))
          .fontSize(14)
          .fontColor($r('app.color.text_tertiary'))
          .padding(16)
      } else {
        List({ space: 2 }) {
          ForEach(this.sessions, (s: SessionMeta) => {
            ListItem() {
              Column({ space: 2 }) {
                Text(s.title.length === 0 ? $r('app.string.drawer_untitled') : s.title)
                  .fontSize(15)
                  .fontColor($r('app.color.text_primary'))
                  .maxLines(1)
                  .textOverflow({ overflow: TextOverflow.Ellipsis })
                  .width('100%')
                Text($r('app.string.drawer_msg_count', s.count))
                  .fontSize(12)
                  .fontColor($r('app.color.text_tertiary'))
              }
              .alignItems(HorizontalAlign.Start)
              .width('100%')
              .padding({ left: 16, right: 16, top: 10, bottom: 10 })
              .backgroundColor(s.id === this.sessionId ? $r('app.color.bg_overlay') : Color.Transparent)
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
        .fontColor($r('app.color.text_tertiary'))
        .padding(16)
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.bg_surface'))
  }

  @Builder
  deleteButton(id: string) {
    Button($r('app.string.settings_delete'))
      .fontSize(14)
      .fontColor($r('app.color.on_brand'))
      .backgroundColor($r('app.color.danger'))
      .height('100%')
      .onClick(() => this.deleteSession(id))
  }

  build() {
    Column() {
      this.header()
      Divider().color($r('app.color.divider'))

      List({ space: 0, scroller: this.scroller }) {
        if (this.messages.length === 0 && !this.streaming) {
          ListItem() {
            this.emptyState()
          }
        }
        ForEach(this.messages, (m: ChatMessage) => {
          ListItem() {
            if (m.role === 'user') {
              this.userBubble(m)
            } else {
              this.assistantBubble(m)
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
    .backgroundColor($r('app.color.bg_page'))
    .bindSheet($$this.showDrawer, this.drawer(), {
      detents: [SheetSize.MEDIUM, SheetSize.LARGE],
      backgroundColor: $r('app.color.bg_surface')
    })
  }
}
