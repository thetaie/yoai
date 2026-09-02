/**
 * One turn, from a transcript to a stream of events.
 *
 * Everything above this file works in ChatMessage objects with display
 * structure; everything below it works in bare {role, content}. This is where
 * the two meet, and it is the only place that knows the native event
 * vocabulary -- so adding an event type later touches one file, not the page.
 */
import hilog from '@ohos.hilog';
import yoai from 'libyoai.so';
import { Canceler, ChatMessage, StreamCallbacks } from '../model/Chat';

/** What actually crosses the ABI: no parts, no timestamps, no transient rows. */
interface WireMessage {
  role: string;
  content: string;
}

export class ChatEngine {
  /**
   * Run one turn against `providerId`, streaming into `cb`.
   *
   * `history` is the whole conversation INCLUDING the user message just sent --
   * the core keeps no session state, so this is the only context the model gets.
   * Returns a handle the UI can stop the turn with.
   *
   * cb.onDone fires exactly once, whatever happens; a failure delivers
   * onError first, so a caller that only implements onDone still ends up in a
   * consistent state.
   */
  static run(providerId: string,
             history: ChatMessage[],
             systemPrompt: string,
             cb: StreamCallbacks): Canceler {
    const canceler: Canceler = {
      stopped: false,
      cancel: () => {}
    };

    const wire: WireMessage[] = [];
    for (const m of history) {
      // Transient rows are the app talking to the user, not part of the
      // conversation; sending them back would have the model answer our own
      // error messages.
      if (m.transient === true) {
        continue;
      }
      if (m.content.length === 0) {
        continue;
      }
      wire.push({ role: m.role, content: m.content });
    }

    if (wire.length === 0) {
      cb.onError('没有可发送的内容');
      cb.onDone(false);
      return canceler;
    }

    let turnId: number = 0;
    try {
      turnId = yoai.nativeChat(providerId, JSON.stringify(wire), systemPrompt,
        (type: string, content: string) => {
          if (type === 'reply') {
            cb.onDelta(content);
          } else if (type === 'thinking') {
            cb.onThinking(content);
          } else if (type === 'usage') {
            if (cb.onUsage !== undefined) {
              cb.onUsage(content);
            }
          } else if (type === 'error') {
            cb.onError(content);
          } else if (type === 'end') {
            cb.onDone(false);
          } else if (type === 'aborted') {
            cb.onDone(true);
          } else {
            // Reserved tool events. Logged rather than shown: an unhandled
            // event is a gap in this file, and the user cannot act on it.
            hilog.info(0x0000, 'yoai', 'unhandled chat event %{public}s', type);
          }
        });
    } catch (err) {
      const e = err as Error;
      cb.onError(e.message.length > 0 ? e.message : '无法启动对话');
      cb.onDone(false);
      return canceler;
    }

    canceler.cancel = () => {
      canceler.stopped = true;
      yoai.nativeChatCancel(turnId);
    };
    return canceler;
  }
}
