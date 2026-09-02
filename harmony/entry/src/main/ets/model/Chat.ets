/**
 * Chat types shared by the UI, the store and the engine.
 *
 * Kept deliberately small: a message is text plus enough display structure to
 * render a reasoning model's output, and nothing else. Attachments, charts and
 * tool steps get their own fields when the features that need them land -- not
 * before, because a field nobody fills is a field everybody has to read past.
 */

/**
 * One ordered piece of an assistant message. `content` alone cannot express a
 * reply whose thinking and prose interleave, so the display structure rides
 * beside it. 'thinking' renders as a collapsible dim block, 'text' as prose.
 */
export interface MessagePart {
  kind: string;      // 'thinking' | 'text'
  content: string;
}

export interface ChatMessage {
  role: string;      // 'system' | 'user' | 'assistant'
  content: string;   // the message TEXT -- what goes back to the model as context

  /**
   * Display structure, assistant messages only (absent => render `content` as
   * one text part). NEVER sent to a provider: the engine maps messages down to
   * bare {role, content} before they cross the C ABI.
   */
  parts?: MessagePart[];

  /**
   * Epoch millis, stamped when the turn is sent (user) or finishes (assistant).
   * Optional because a stamp minted at load time would read as "just now" for a
   * conversation from last week, and that is worse than showing nothing.
   */
  ts?: number;

  /** Assistant turns: which model wrote it, as the picker's own label. */
  model?: string;

  /**
   * Ephemeral: drawn in the thread, never sent to a model and never written to
   * disk. What the app says to the user (an error, a notice) is on screen
   * because that is where it was needed -- it is not part of the conversation,
   * and replaying it as context on every later turn would be absurd.
   */
  transient?: boolean;
}

/** Streaming callbacks for one turn. */
export interface StreamCallbacks {
  onDelta: (text: string) => void;
  onThinking: (text: string) => void;
  /** Fires exactly once, whether the turn finished, failed or was cancelled. */
  onDone: (aborted: boolean) => void;
  onError: (message: string) => void;
  /** Token/cost summary as a JSON object string; optional. */
  onUsage?: (json: string) => void;
}

/**
 * Cancel handle for an in-flight turn.
 *
 * `stopped` covers the window BEFORE the turn has a native id to cancel -- the
 * engine awaits the stored key first, and a stop during that gap would
 * otherwise still fire the request.
 */
export interface Canceler {
  cancel: () => void;
  stopped: boolean;
}
