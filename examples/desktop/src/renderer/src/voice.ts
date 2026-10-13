/**
 * A voice conversation, from the page's side: the microphone button's two
 * modes, and what each utterance does to the conversation.
 *
 * **Walkie-talkie ('talk')**: a click starts recording and stops whatever is
 * being said; the next click sends it. Nothing plays while it records, so
 * there is no echo to take out.
 *
 * **Full duplex ('duplex')**: a click opens a session that listens until the
 * next click, the reply included. The page streams the microphone and what
 * the speaker plays; the main process takes the echo out, finds where speech
 * starts and stops, and recognizes it (main/duplex.ts). Here each utterance is
 * judged, as docs/slides.md lays out under "Barge-in: a second sentence
 * mid-reply":
 *
 *   - A voice over the reply pauses it. If it turns out to be no words, or the
 *     reply's own words heard back, the reply goes on.
 *   - Nothing being answered: an utterance with nothing to answer yet ("我想问
 *     一下") is shown and held, and the next is joined to it; one that can be
 *     answered is sent. The judgment is the core's rules (yoai_turn_cue) and,
 *     in the turn itself, the chat model answering <wait>.
 *   - A reply under way: a correction ("不对，是杭州") stops it, keeps what was
 *     heard of it, and sends what was meant -- joined to the request instead
 *     when none of the reply was heard; a stop ("算了") just stops it; anything
 *     else ("还有杭州") is added, and sent once this reply has been heard out.
 */
import type { DuplexEvent, MicMode, Transcript } from '@shared/api'
import { player } from './audio'
import { joinBlocks, Mic, MIC_RATE, openMic } from './mic'
import * as reader from './reader'

/** What the page needs to give a voice conversation. App implements it. */
export interface VoiceHost {
  /** A turn is being written. */
  writing(): boolean
  /** The reply being written or heard, as far as it has got; '' when none. */
  replyText(): string
  /** The last user message's text; '' when there is none. */
  lastUserText(): string
  /**
   * Send `text` as the user and read the reply aloud. `live`: the model may
   * answer <wait> alone. `join`: replace the last user message rather than add
   * one. Resolves when the turn is over: 'wait' when the model waited.
   */
  send(text: string, live: boolean, join: boolean): Promise<'done' | 'stopped' | 'wait'>
  /** Show `text` as the user's, with no reply yet, or replace the last user message. */
  show(text: string, join: boolean): void
  /**
   * Stop the reply and the speech. The reply keeps `heard` of its text -- or
   * goes, when '' -- or all of it, when null. Resolves once the turn is over.
   */
  stopReply(heard: string | null): Promise<void>
  /** A line from the app in the thread, never sent to a model. */
  note(text: string): void
}

export type MicState =
  | 'idle'
  /** talk: recording; the next click sends. */
  | 'recording'
  /** talk: what was recorded is being recognized. */
  | 'recognizing'
  /** duplex: open, and quiet. */
  | 'listening'
  /** duplex: a voice now. */
  | 'hearing'
  /** duplex: a voice just stopped, and is being recognized. */
  | 'thinking'

/** Shorter than this is a click, not a recording. */
const MIN_RECORDING_SECONDS = 0.3
/** How long a held utterance waits for the rest before it is sent as it is. */
const HOLD_MS = 4000
/** Characters a second of speech, while a reply is still being made; see heard(). */
const SPOKEN_CHARS_PER_SECOND = 5
/** The reference kept beyond what the next block needs: 200ms at 24 kHz. */
const REF_SLACK = 4800

export class VoiceController {
  private host: VoiceHost
  private state: MicState = 'idle'
  private readonly watchers = new Set<() => void>()

  private mode: MicMode = 'talk'
  private mic: Mic | null = null
  private blocks: Int16Array[] = []

  // Full duplex only.
  private offEvents: (() => void) | null = null
  private offTap: (() => void) | null = null
  private refs: Float32Array[] = []
  private refLength = 0
  /** The player was paused because a voice was heard over it. */
  private pausedByVoice = false
  /** An utterance with nothing to answer yet, shown and held for the rest. */
  private held = ''
  private holdTimer: ReturnType<typeof setTimeout> | null = null
  /** Utterances added to a reply under way, sent once it has been heard out. */
  private added: string[] = []
  private offPlayer: (() => void) | null = null

  constructor(host: VoiceHost) {
    this.host = host
  }

  /** The page re-renders with new functions; keep the latest. */
  setHost(host: VoiceHost): void {
    this.host = host
  }

  watch = (listener: () => void): (() => void) => {
    this.watchers.add(listener)
    return () => {
      this.watchers.delete(listener)
    }
  }

  current = (): MicState => this.state

  private set(state: MicState): void {
    if (state !== this.state) {
      this.state = state
      for (const w of this.watchers) {
        w()
      }
    }
  }

  /** The microphone button. */
  async click(): Promise<void> {
    if (this.state === 'idle') {
      // Asked now rather than remembered: the model loads in the background
      // after the window opens, and the mode is changed on the settings page.
      const status = await window.yoai.asr.status()
      if (!status.loaded) {
        this.host.note(
          status.loading ? '语音识别模型还在载入，请稍候' : '先在设置里选择语音识别模型（SenseVoice）'
        )
        return
      }
      this.mode = status.mode
      if (this.mode === 'talk') {
        await this.startRecording()
      } else {
        await this.openSession()
      }
    } else if (this.state === 'recording') {
      await this.sendRecording()
    } else if (this.mode === 'duplex') {
      this.closeSession()
    }
  }

  /** Let go of the microphone, whatever it is doing. */
  close(): void {
    if (this.mode === 'duplex') {
      this.closeSession()
    } else {
      this.mic?.close()
      this.mic = null
      this.blocks = []
      this.set('idle')
    }
  }

  //----------------------------------------------------------------------------
  // Walkie-talkie
  //----------------------------------------------------------------------------

  private async startRecording(): Promise<void> {
    // Whoever talks stops whoever was: the reply is cut where it was heard.
    if (this.host.writing() || player.active()) {
      void this.host.stopReply(null)
    }
    this.blocks = []
    try {
      this.mic = await openMic((pcm) => this.blocks.push(pcm))
    } catch (e) {
      this.host.note(`无法打开麦克风${e instanceof Error ? `（${e.message}）` : ''}`)
      return
    }
    this.set('recording')
  }

  private async sendRecording(): Promise<void> {
    this.mic?.close()
    this.mic = null
    const pcm = joinBlocks(this.blocks)
    this.blocks = []
    if (pcm.length < MIN_RECORDING_SECONDS * MIC_RATE) {
      this.set('idle')
      return
    }
    this.set('recognizing')
    const t = await window.yoai.asr.transcribe(pcm)
    this.set('idle')
    if (t.error.length > 0) {
      this.host.note(`识别失败：${t.error}`)
    } else if (t.text.length === 0) {
      this.host.note('没有听清，请再说一次')
    } else {
      void this.host.send(t.text, false, false)
    }
  }

  //----------------------------------------------------------------------------
  // Full duplex
  //----------------------------------------------------------------------------

  private async openSession(): Promise<void> {
    const reason = await window.yoai.duplex.start()
    if (reason.length > 0) {
      this.host.note(`无法开始语音对话：${reason}`)
      return
    }
    this.offEvents = window.yoai.duplex.onEvent((ev) => this.onEvent(ev))
    this.refs = []
    this.refLength = 0
    this.offTap = player.tap((pcm) => {
      this.refs.push(pcm)
      this.refLength += pcm.length
    })
    this.offPlayer = player.watch(() => this.sendAdded())
    try {
      this.mic = await openMic((pcm) => this.pump(pcm))
    } catch (e) {
      this.closeSession()
      this.host.note(`无法打开麦克风${e instanceof Error ? `（${e.message}）` : ''}`)
      return
    }
    this.set('listening')
  }

  private closeSession(): void {
    this.mic?.close()
    this.mic = null
    this.offTap?.()
    this.offTap = null
    this.offEvents?.()
    this.offEvents = null
    this.offPlayer?.()
    this.offPlayer = null
    void window.yoai.duplex.stop()
    if (this.pausedByVoice) {
      player.resume()
      this.pausedByVoice = false
    }
    this.clearHold()
    this.held = ''
    this.added = []
    this.set('idle')
  }

  /**
   * One microphone block out, with what the speaker played over the same
   * span: the reference the canceller pairs it with. The two arrive on their
   * own clocks, so the reference is taken as far as it has come and padded
   * with silence -- which is also what the speaker plays while paused, when
   * none comes at all.
   */
  private pump(mic: Int16Array): void {
    const need = Math.round((mic.length * 24000) / MIC_RATE)
    while (this.refLength > need + REF_SLACK && this.refs.length > 0) {
      const drop = this.refs.shift() as Float32Array
      this.refLength -= drop.length
    }
    const ref = new Float32Array(need)
    let at = 0
    while (at < need && this.refs.length > 0) {
      const head = this.refs[0]
      const n = Math.min(head.length, need - at)
      ref.set(head.subarray(0, n), at)
      at += n
      if (n === head.length) {
        this.refs.shift()
      } else {
        this.refs[0] = head.subarray(n)
      }
      this.refLength -= n
    }
    window.yoai.duplex.audio(mic, ref)
  }

  private onEvent(ev: DuplexEvent): void {
    if (ev.type === 'speech') {
      this.set('hearing')
      // A held utterance waits for the user's silence, not the clock: while
      // they talk, the rest is still coming.
      this.clearHold()
      if (player.active() && !player.paused()) {
        player.pause()
        this.pausedByVoice = true
      }
    } else if (ev.type === 'silence') {
      this.set('thinking')
    } else if (ev.type === 'utterance' && ev.transcript !== undefined) {
      this.set('listening')
      void this.onUtterance(ev.transcript)
    } else if (ev.type === 'error') {
      this.host.note(`语音对话已停止：${ev.error ?? ''}`)
      this.closeSession()
    }
  }

  private resumeReply(): void {
    if (this.pausedByVoice) {
      player.resume()
      this.pausedByVoice = false
    }
  }

  private async onUtterance(t: Transcript): Promise<void> {
    const reply = this.host.replyText()
    const replying = this.host.writing() || player.active()

    // Nothing said, or the reply heard back through the canceller: go on, and
    // a held utterance goes on waiting from here. A single character is noise
    // the gate let through -- recognition makes "我" or "这" of a burst --
    // unless it is one that stops the reply.
    if (
      t.text.length === 0 ||
      (t.event.length > 0 && t.event !== 'Speech') ||
      (bare(t.text).length < 2 && t.cue !== 'stop') ||
      echoes(t.text, reply)
    ) {
      this.resumeReply()
      if (this.held.length > 0) {
        this.hold(this.held)
      }
      return
    }

    if (replying) {
      if (t.cue === 'stop' || t.cue === 'correct') {
        this.pausedByVoice = false
        const heard = heardOf(reply)
        const request = this.host.lastUserText()
        await this.host.stopReply(heard)
        if (t.cue === 'correct') {
          // None of the reply was heard: the correction belongs to the request.
          if (heard.length === 0 && request.length > 0) {
            this.sendNow(joined(request, t.text), true)
          } else {
            this.sendNow(t.text, false)
          }
        }
        return
      }
      // Added to the request: this reply goes on, and the addition follows it.
      this.resumeReply()
      this.added.push(t.text)
      this.sendAdded()
      return
    }

    this.clearHold()
    const text = this.held.length > 0 ? joined(this.held, t.text) : t.text
    const join = this.held.length > 0
    if (t.cue === 'unfinished') {
      this.host.show(text, join)
      this.hold(text)
      return
    }
    this.held = ''
    this.sendNow(text, join)
  }

  /**
   * Send a turn, and hold it if the model answers <wait>. Only a single
   * utterance is a live turn, which the model may answer that way: one joined
   * to a held start has already been judged complete, and begins with exactly
   * the kind of opener the live-turn prompt names -- Qwen3.5-2B answered
   * "我想问一下。明天上海天气怎么样？" with <wait>.
   */
  private sendNow(text: string, join: boolean): void {
    void this.host.send(text, !join, join).then((how) => {
      if (how === 'wait' && this.mic !== null) {
        this.hold(text)
      }
    })
  }

  /** Show `text` as waiting for the rest; sent as it is if none comes. */
  private hold(text: string): void {
    this.held = text
    this.clearHold()
    this.holdTimer = setTimeout(() => {
      this.holdTimer = null
      const held = this.held
      this.held = ''
      if (held.length > 0 && this.mic !== null) {
        // Not live: asked again, the model would only wait again.
        void this.host.send(held, false, true)
      }
    }, HOLD_MS)
  }

  private clearHold(): void {
    if (this.holdTimer !== null) {
      clearTimeout(this.holdTimer)
      this.holdTimer = null
    }
  }

  /** Send what was added to the last reply, once that has been heard out. */
  private sendAdded(): void {
    if (this.added.length === 0 || this.host.writing() || player.active()) {
      return
    }
    const text = this.added.join('，')
    this.added = []
    this.sendNow(text, false)
  }
}

//------------------------------------------------------------------------------

/** Two utterances as one request: the second after the first, as if unbroken. */
function joined(first: string, second: string): string {
  return /[，。！？、,.!?；;]$/.test(first) ? first + second : `${first}，${second}`
}

/** Letters and digits only, for comparing what was said. */
function bare(text: string): string {
  return text.replace(/[\s\p{P}\p{S}]/gu, '').toLowerCase()
}

/**
 * Whether `heard` is the reply coming back through the microphone: most of its
 * pairs of characters occur in the reply. The canceller leaves echo it cannot
 * model, and recognition writes that down like anything else.
 */
export function echoes(heard: string, reply: string): boolean {
  const h = bare(heard)
  const r = bare(reply)
  if (h.length < 2 || r.length < 2) {
    return false
  }
  let found = 0
  for (let i = 0; i + 1 < h.length; i++) {
    if (r.includes(h.slice(i, i + 2))) {
      found++
    }
  }
  return found / (h.length - 1) >= 0.6
}

/**
 * How much of `reply` has been heard: its first sentences, up to where the
 * speaker has got. Approximate -- the engine does not say which character it
 * is reading -- so the rate is the reply's own once it has all been made, and
 * a typical one before; and it is cut back to the last whole sentence.
 */
function heardOf(reply: string): string {
  const played = player.playedSeconds()
  const made = reader.produced()
  const rate =
    made.done && made.seconds > 1 ? reply.length / made.seconds : SPOKEN_CHARS_PER_SECOND
  const upTo = Math.min(reply.length, Math.round(played * rate))
  const head = reply.slice(0, upTo)
  const end = Math.max(...['。', '！', '？', '!', '?', '；', ';', '\n'].map((c) => head.lastIndexOf(c)))
  return end >= 0 ? reply.slice(0, end + 1).trim() : ''
}
