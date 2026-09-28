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
  '你是一个简洁、直接的助手，回答用简体中文。\n' +
  '只说你确定的事实。' +
  '涉及具体的地名、景点、机构、人名、日期、数字时，没有把握就不要列出，直接说明这一点你不确定；' +
  '不要编造，也不要用名字相近的东西凑数。' +
  '宁可少说，不要说错。'

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
