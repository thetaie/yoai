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
 *
 * The default is the core's, not this file's (src/llm/prompt/prompts.cpp).
 * chat.json holds a prompt only once the user has written one; until then each
 * turn goes out with the core's default as it is in this build, so a new
 * default reaches everyone who never changed it. A copy kept here is what
 * drifted before.
 */
import type { ChatSettings, ChatSettingsPatch } from '../shared/api'
import { core } from './core'
import { dataFile, readJson, writeJson } from './jsonfile'

const FILE = 'chat.json'

/** What chat.json holds. No systemPrompt: the user never wrote one. */
interface Stored {
  systemPrompt?: string
}

/** The user's own prompt, or null when they never wrote one. '' is one they cleared. */
function stored(): string | null {
  const s = readJson<Stored>(dataFile(FILE), {})
  return typeof s.systemPrompt === 'string' ? s.systemPrompt : null
}

/** The core's default, or null when the addon will not load. */
function builtin(): string | null {
  try {
    return core().nativePromptGet('system')
  } catch {
    return null
  }
}

export function get(): ChatSettings {
  const own = stored()
  if (own !== null) {
    return { systemPrompt: own, custom: true }
  }
  return { systemPrompt: builtin() ?? '', custom: false }
}

/**
 * Store the user's prompt, or forget it with null.
 *
 * A text equal to the default is forgotten too. The settings box saves on blur,
 * so one clicked into and left as it was would otherwise pin this build's
 * default, and the next one would never reach that user.
 */
export function set(patch: ChatSettingsPatch): ChatSettings {
  if (patch.systemPrompt === null) {
    writeJson(dataFile(FILE), {})
  } else if (typeof patch.systemPrompt === 'string') {
    const isDefault = patch.systemPrompt === builtin()
    writeJson(dataFile(FILE), isDefault ? {} : { systemPrompt: patch.systemPrompt })
  }
  return get()
}

/**
 * The system prompt one turn is sent with: the user's own or the core's
 * default, the spoken-style paragraph after it when the reply will be heard,
 * and the core's line saying all of it is internal.
 *
 * The turn and the local model's warm-up both take it from here, and the core
 * builds it from the same two inputs, so they agree to the token -- a warm-up
 * spent on a prompt nobody sends is wasted.
 */
export function turnPrompt(reading: boolean): string {
  return core().nativePromptTurnSystem(stored(), reading)
}
