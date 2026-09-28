/**
 * One turn, from a transcript to a stream of events.
 *
 * A port of harmony/entry/src/main/ets/core/ChatEngine.ets, and it holds the
 * same line: everything above this file works in ChatMessage objects with
 * display structure; everything below it works in bare {role, content}. This is
 * where the two meet, and it is the only place in the renderer that knows the
 * native event vocabulary -- so adding an event type later touches one file,
 * not a component.
 */
import type { Canceler, ChatEvent, ChatMessage, StreamCallbacks, WireMessage } from '@shared/chat'

/**
 * Run one turn against `providerId`, streaming into `cb`.
 *
 * `history` is the whole conversation INCLUDING the user message just sent --
 * the core keeps no session state, so this is the only context the model gets.
 * Returns a handle the UI can stop the turn with.
 *
 * cb.onDone fires exactly once, whatever happens; a failure delivers onError
 * first, so a caller that only implements onDone still ends up in a consistent
 * state.
 */
export function runTurn(
  providerId: string,
  history: ChatMessage[],
  systemPrompt: string,
  cb: StreamCallbacks
): Canceler {
  const canceler: Canceler = { stopped: false, cancel: () => {} }

  const wire: WireMessage[] = []
  for (const m of history) {
    // Transient rows are the app talking to the user, not part of the
    // conversation; sending them back would have the model answer our own
    // error messages.
    if (m.transient === true || m.content.length === 0) {
      continue
    }
    wire.push({ role: m.role, content: m.content })
  }

  if (wire.length === 0) {
    cb.onError('没有可发送的内容')
    cb.onDone(false)
    return canceler
  }

  let turnId = -1
  let finished = false

  /**
   * Events that arrived before the turn had an id.
   *
   * The main process replies with the id before it forwards anything, and both
   * travel the same pipe in order, so this should always stay empty. It is here
   * because "should" is doing real work in that sentence and the cost of being
   * wrong is a lost first token, which reads as the model swallowing a word.
   */
  const pending: ChatEvent[] = []

  const deliver = (ev: ChatEvent): void => {
    if (finished) {
      return
    }
    switch (ev.type) {
      case 'reply':
        cb.onDelta(ev.content)
        break
      case 'thinking':
        cb.onThinking(ev.content)
        break
      case 'usage':
        cb.onUsage?.(ev.content)
        break
      case 'error':
        cb.onError(ev.content)
        break
      case 'end':
      case 'aborted':
        finished = true
        unsubscribe()
        cb.onDone(ev.type === 'aborted')
        break
      default:
        // Reserved tool events. Logged rather than shown: an unhandled event is
        // a gap in this file, and the user cannot act on it.
        console.info(`[yoai] unhandled chat event ${ev.type}`)
    }
  }

  const unsubscribe = window.yoai.chat.onEvent((ev) => {
    if (turnId < 0) {
      pending.push(ev)
      return
    }
    if (ev.turnId === turnId) {
      deliver(ev)
    }
  })

  window.yoai.chat
    .start(providerId, wire, systemPrompt)
    .then((id) => {
      turnId = id

      // The user pressed stop while the turn was still being started. The
      // request is already out, so cancelling is the only way to stop it; the
      // 'aborted' event still comes back and closes the turn normally.
      if (canceler.stopped) {
        void window.yoai.chat.cancel(id)
      }

      for (const ev of pending.splice(0)) {
        if (ev.turnId === id) {
          deliver(ev)
        }
      }
    })
    .catch((err: unknown) => {
      finished = true
      unsubscribe()
      const message = err instanceof Error ? err.message : String(err)
      cb.onError(message.length > 0 ? message : '无法启动对话')
      cb.onDone(false)
    })

  canceler.cancel = () => {
    canceler.stopped = true
    if (turnId >= 0) {
      void window.yoai.chat.cancel(turnId)
    }
  }
  return canceler
}

/**
 * A session id: the clock plus a random tail.
 *
 * The clock alone collides when two sessions are created in the same
 * millisecond, which "new chat" twice in a row does. The alphabet is checked
 * again in the main process, where the id becomes a file name.
 */
export function newSessionId(): string {
  return `${Date.now()}-${Math.floor(Math.random() * 100000)}`
}
