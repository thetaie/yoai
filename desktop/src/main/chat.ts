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

/** Empty, which is what every turn sent before this setting existed. */
const EMPTY: ChatSettings = { systemPrompt: '' }

export function get(): ChatSettings {
  const s = readJson<ChatSettings>(dataFile(FILE), EMPTY)
  return { systemPrompt: typeof s.systemPrompt === 'string' ? s.systemPrompt : '' }
}

export function set(patch: Partial<ChatSettings>): ChatSettings {
  const next: ChatSettings = {
    systemPrompt: typeof patch.systemPrompt === 'string' ? patch.systemPrompt : get().systemPrompt
  }
  writeJson(dataFile(FILE), next)
  return next
}
