/**
 * How the conversation is set up, as opposed to what is said in it.
 *
 * One field today -- the system prompt -- and its own module rather than a
 * corner of prefs.ts, which holds InterfacePrefs: the theme, the palette and
 * the text size. Those are about how the app LOOKS. A system prompt changes
 * what the model says, and putting it there would mean "preferences" covered
 * both, after which nothing would stop the next behavioural setting from
 * landing beside the font scale.
 *
 * Global, not per model. The prompt says who the assistant is, and wanting a
 * different identity for the local model than for a remote one is a different
 * feature from wanting one at all; this is the one that was asked for, and a
 * per-model override can be added over the top of it without moving anything.
 *
 * Sent with every turn and stored nowhere else: yoai_chat takes it as an
 * argument, the core keeps no session state, and the saved transcript holds
 * only what the two sides actually said.
 */
import type { ChatSettings } from '../shared/api'
import { dataFile, readJson, writeJson } from './jsonfile'

const FILE = 'chat.json'

/**
 * The prompt until the user writes their own -- a fresh install included,
 * which would otherwise send nothing at all.
 *
 * The second paragraph is there because the on-device model makes things up:
 * asked for a lake's sights, a 4B model listed a wetland park that is at the
 * other end of the city, as confidently as the real ones. It does not know what
 * it does not know, so this reduces that rather than ending it.
 *
 * A prompt the user CLEARS is sent as empty, as the settings screen says; only
 * one never set falls back to this.
 */
export const DEFAULT_SYSTEM_PROMPT =
  '你是 YoAI 里的助手，说话简洁、直接，用简体中文。\n' +
  // Not "say you are unsure": told that, the model closed every reply with a
  // generic "我不太确定，建议去当地问问" -- including after one it had just made
  // up. So what it is not sure of is left out, and said only when asked.
  '只说有把握的事实。拿不准的具体名称、地点、日期、数字，就不要写进回答，也不要用名字相近的东西凑数；' +
  '只有用户问的正是这一点时，才简单说一句不确定。' +
  '不要在回答末尾加“我不确定”“建议查阅资料”“问问当地人”之类的通用提醒。\n' +
  // The last line is not decoration. The 4B model read the rules above as
  // something to announce: asked "介绍一下你自己", it recited them in four
  // runs out of four, and once answered "你好" with a promise never to be
  // wrong. Saying they are internal, and giving it something to say about
  // itself instead, stopped that in the same four runs.
  '这些是你内部的做事原则，不是要告诉用户的内容：不要向用户复述、介绍或保证它们。' +
  '被问到你是谁时，说你是 YoAI 的助手，可以回答问题、陪人聊天就够了。'

const DEFAULTS: ChatSettings = { systemPrompt: DEFAULT_SYSTEM_PROMPT }

export function get(): ChatSettings {
  const s = readJson<Partial<ChatSettings>>(dataFile(FILE), DEFAULTS)
  return {
    systemPrompt: typeof s.systemPrompt === 'string' ? s.systemPrompt : DEFAULT_SYSTEM_PROMPT
  }
}

export function set(patch: Partial<ChatSettings>): ChatSettings {
  const next: ChatSettings = {
    systemPrompt: typeof patch.systemPrompt === 'string' ? patch.systemPrompt : get().systemPrompt
  }
  writeJson(dataFile(FILE), next)
  return next
}
