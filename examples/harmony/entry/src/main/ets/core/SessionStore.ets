/**
 * Locally persisted chat sessions -- what the history drawer lists.
 *
 * There is nothing to sync with: YoAI talks straight to the provider the user
 * configured and has no account, so history is purely local and stays that way.
 *
 * Storage layout: one preferences store, with the session INDEX (ids, titles,
 * timestamps -- small) under one key and each session's messages under their own
 * `msg.<id>`. Split for two reasons: the drawer needs only the index to render,
 * so opening it never deserializes megabytes of transcript, and one huge
 * conversation cannot slow down every other session's read.
 *
 * Preferences holds everything in memory once loaded, so this suits tens of
 * sessions, not thousands. MAX_SESSIONS trims the oldest rather than letting
 * that grow without bound; a relational store is the answer if this ever needs
 * search or real paging.
 */
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import hilog from '@ohos.hilog';
import { ChatMessage } from '../model/Chat';

const STORE_NAME: string = 'sessions';
const KEY_INDEX: string = 'index';
const KEY_MSG_PREFIX: string = 'msg.';

/** Sessions past this many are dropped, oldest first, when a new one is saved. */
const MAX_SESSIONS: number = 100;

/** Characters of the first user message kept as the session title. */
const TITLE_LEN: number = 60;

/** A drawer row: enough to list and sort, without the transcript. */
export interface SessionMeta {
  id: string;
  title: string;
  updatedAt: number;   // epoch millis; the list sorts on this
  count: number;       // messages, shown as a subtitle
}

export class SessionStore {
  private static store: preferences.Preferences | null = null;

  private static async prefs(ctx: common.UIAbilityContext): Promise<preferences.Preferences> {
    if (SessionStore.store === null) {
      SessionStore.store = await preferences.getPreferences(ctx, STORE_NAME);
    }
    return SessionStore.store;
  }

  static newId(): string {
    // Time plus a random tail: the clock alone collides when two sessions are
    // created in the same millisecond, which "new chat" twice in a row does.
    return Date.now().toString() + '-' + Math.floor(Math.random() * 100000).toString();
  }

  /** The drawer's list, newest first. */
  static async index(ctx: common.UIAbilityContext): Promise<SessionMeta[]> {
    const p: preferences.Preferences = await SessionStore.prefs(ctx);
    const raw: string = await p.get(KEY_INDEX, '[]') as string;
    let list: SessionMeta[] = [];
    try {
      list = JSON.parse(raw) as SessionMeta[];
    } catch (err) {
      hilog.error(0x0000, 'yoai', 'session index unreadable, starting empty');
      return [];
    }
    list.sort((a: SessionMeta, b: SessionMeta) => b.updatedAt - a.updatedAt);
    return list;
  }

  private static async writeIndex(ctx: common.UIAbilityContext, list: SessionMeta[]): Promise<void> {
    const p: preferences.Preferences = await SessionStore.prefs(ctx);
    await p.put(KEY_INDEX, JSON.stringify(list));
    await p.flush();
  }

  static async messages(ctx: common.UIAbilityContext, id: string): Promise<ChatMessage[]> {
    const p: preferences.Preferences = await SessionStore.prefs(ctx);
    const raw: string = await p.get(KEY_MSG_PREFIX + id, '[]') as string;
    try {
      return JSON.parse(raw) as ChatMessage[];
    } catch (err) {
      hilog.error(0x0000, 'yoai', 'session %{public}s unreadable', id);
      return [];
    }
  }

  /**
   * Save (or update) one session. Transient messages are dropped on the way in:
   * an error notice is worth showing once, not worth keeping forever.
   *
   * A session with nothing left to save is REMOVED rather than stored empty --
   * that is what makes "start a chat, send nothing, leave" not litter the
   * drawer with blank rows.
   */
  static async save(ctx: common.UIAbilityContext, id: string, messages: ChatMessage[]): Promise<void> {
    const keep: ChatMessage[] = [];
    for (const m of messages) {
      if (m.transient !== true) {
        keep.push(m);
      }
    }
    if (keep.length === 0) {
      await SessionStore.remove(ctx, id);
      return;
    }

    const p: preferences.Preferences = await SessionStore.prefs(ctx);
    await p.put(KEY_MSG_PREFIX + id, JSON.stringify(keep));

    const list: SessionMeta[] = await SessionStore.index(ctx);
    const next: SessionMeta[] = [];
    for (const s of list) {
      if (s.id !== id) {
        next.push(s);
      }
    }
    next.unshift({
      id: id,
      title: SessionStore.titleOf(keep),
      updatedAt: Date.now(),
      count: keep.length
    });

    // Trim the tail, deleting each dropped session's transcript too -- an
    // orphaned msg.<id> would sit in the store forever, invisible.
    while (next.length > MAX_SESSIONS) {
      const dropped: SessionMeta | undefined = next.pop();
      if (dropped !== undefined) {
        await p.delete(KEY_MSG_PREFIX + dropped.id);
      }
    }

    await SessionStore.writeIndex(ctx, next);
  }

  static async remove(ctx: common.UIAbilityContext, id: string): Promise<void> {
    const p: preferences.Preferences = await SessionStore.prefs(ctx);
    await p.delete(KEY_MSG_PREFIX + id);
    const list: SessionMeta[] = await SessionStore.index(ctx);
    const next: SessionMeta[] = [];
    for (const s of list) {
      if (s.id !== id) {
        next.push(s);
      }
    }
    await SessionStore.writeIndex(ctx, next);
  }

  static async clear(ctx: common.UIAbilityContext): Promise<void> {
    const p: preferences.Preferences = await SessionStore.prefs(ctx);
    const list: SessionMeta[] = await SessionStore.index(ctx);
    for (const s of list) {
      await p.delete(KEY_MSG_PREFIX + s.id);
    }
    await SessionStore.writeIndex(ctx, []);
  }

  /** The first user message, clipped. Falls back to the first message of any role. */
  private static titleOf(messages: ChatMessage[]): string {
    for (const m of messages) {
      if (m.role === 'user' && m.content.length > 0) {
        return SessionStore.clip(m.content);
      }
    }
    return messages.length > 0 ? SessionStore.clip(messages[0].content) : '';
  }

  private static clip(text: string): string {
    const oneLine: string = text.replace(/\s+/g, ' ').trim();
    return oneLine.length <= TITLE_LEN ? oneLine : oneLine.substring(0, TITLE_LEN) + '…';
  }
}
