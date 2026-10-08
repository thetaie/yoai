/**
 * Chat types shared by the renderer, the main process and the preload bridge.
 *
 * A port of examples/harmony/entry/src/main/ets/model/Chat.ets, and kept a port: when a
 * field is added there it should appear here with the same name and the same
 * meaning, so a transcript written by one client reads in the other.
 *
 * Kept deliberately small: a message is text plus enough display structure to
 * render a reasoning model's output, and nothing else.
 */

/**
 * One ordered piece of an assistant message. `content` alone cannot express a
 * reply whose thinking and prose interleave, so the display structure rides
 * beside it. 'thinking' renders as a collapsible dim block, 'text' as prose.
 */
export interface MessagePart {
  kind: 'thinking' | 'text'
  content: string
}

export interface ChatMessage {
  role: 'system' | 'user' | 'assistant'
  /** The message TEXT -- what goes back to the model as context. */
  content: string

  /**
   * Display structure, assistant messages only (absent => render `content` as
   * one text part). NEVER sent to a provider: the engine maps messages down to
   * bare {role, content} before they cross the C ABI.
   */
  parts?: MessagePart[]

  /**
   * Epoch millis, stamped when the turn is sent (user) or finishes (assistant).
   * Optional because a stamp minted at load time would read as "just now" for a
   * conversation from last week, and that is worse than showing nothing.
   */
  ts?: number

  /** Assistant turns: which model wrote it, as the picker's own label. */
  model?: string

  /**
   * Ephemeral: drawn in the thread, never sent to a model and never written to
   * disk. What the app says to the user (an error, a notice) is on screen
   * because that is where it was needed -- it is not part of the conversation,
   * and replaying it as context on every later turn would be absurd.
   */
  transient?: boolean
}

/** What actually crosses the ABI: no parts, no timestamps, no transient rows. */
export interface WireMessage {
  role: string
  content: string
}

/**
 * One event from a turn, exactly as the C ABI names it, forwarded verbatim from
 * the main process. The vocabulary is documented on yoai_chat_handler in
 * src/yoai.h; 'end' and 'aborted' are the bridge's own terminal events.
 */
export interface ChatEvent {
  turnId: number
  type: string
  content: string
}

/** Streaming callbacks for one turn. */
export interface StreamCallbacks {
  onDelta: (text: string) => void
  onThinking: (text: string) => void
  /** Fires exactly once, whether the turn finished, failed or was cancelled. */
  onDone: (aborted: boolean) => void
  onError: (message: string) => void
  /** Token/cost summary as a JSON object string; optional. */
  onUsage?: (json: string) => void
}

/** The `usage` event's payload, once parsed. Every field may be absent. */
export interface Usage {
  model?: string
  inputTokens?: number
  outputTokens?: number
  thoughtTokens?: number
  totalTokens?: number
  totalCost?: number
}

/**
 * Cancel handle for an in-flight turn.
 *
 * `stopped` covers the window BEFORE the turn has a native id to cancel. On
 * HarmonyOS that gap is an awaited key read; here it is the IPC round trip that
 * starts the turn, and a stop during it would otherwise still let the request
 * go out.
 */
export interface Canceler {
  cancel: () => void
  stopped: boolean
}
