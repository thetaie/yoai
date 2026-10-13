/**
 * How the conversation is set up, as opposed to what is said in it: the
 * system prompt.
 *
 * A port of examples/desktop/src/main/chat.ts. The default is the core's, not
 * this file's (src/llm/prompt/prompts.cpp). Preferences hold a prompt only once
 * the user has written one; until then each turn goes out with the core's
 * default as it is in this build, so a new default reaches everyone who never
 * changed it. A copy kept here is what drifted before -- this app used to send
 * no system prompt at all.
 *
 * Global, not per model, and stored nowhere else: nativeChat takes it as an
 * argument, the core keeps no session state, and the saved transcript holds
 * only what the two sides actually said.
 */
import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import yoai from 'libyoai.so';

const STORE_NAME: string = 'chat';
const KEY_PROMPT: string = 'systemPrompt';

/** The system prompt as the settings screen shows it. */
export interface PromptSetting {
  /** The user's own, or the core's default while `custom` is false. '' sends none. */
  text: string;
  custom: boolean;
}

export class ChatSettings {
  private static store: preferences.Preferences | null = null;

  private static async prefs(ctx: common.Context): Promise<preferences.Preferences> {
    if (ChatSettings.store === null) {
      ChatSettings.store = await preferences.getPreferences(ctx, STORE_NAME);
    }
    return ChatSettings.store;
  }

  /** The user's own prompt, or null when they never wrote one. '' is one they cleared. */
  private static async stored(ctx: common.Context): Promise<string | null> {
    const p: preferences.Preferences = await ChatSettings.prefs(ctx);
    if (!(await p.has(KEY_PROMPT))) {
      return null;
    }
    return await p.get(KEY_PROMPT, '') as string;
  }

  /** The core's default system prompt. */
  static builtin(): string {
    const text: string | null = yoai.nativePromptGet('system');
    return text === null ? '' : text;
  }

  static async get(ctx: common.Context): Promise<PromptSetting> {
    const own: string | null = await ChatSettings.stored(ctx);
    if (own !== null) {
      return { text: own, custom: true };
    }
    return { text: ChatSettings.builtin(), custom: false };
  }

  /**
   * Store the user's prompt, or forget it with null.
   *
   * A text equal to the default is forgotten too. The box saves when it loses
   * focus, so one tapped into and left as it was would otherwise pin this
   * build's default, and the next one would never reach that user.
   */
  static async set(ctx: common.Context, text: string | null): Promise<PromptSetting> {
    const p: preferences.Preferences = await ChatSettings.prefs(ctx);
    if (text === null || text === ChatSettings.builtin()) {
      await p.delete(KEY_PROMPT);
    } else {
      await p.put(KEY_PROMPT, text);
    }
    await p.flush();
    return ChatSettings.get(ctx);
  }

  /**
   * The system prompt one turn is sent with: the user's own or the core's
   * default, the spoken-style paragraph after it when the reply will be heard,
   * and the core's line saying all of it is internal.
   *
   * The turn and the on-device model's warm-up both take it from here, and the
   * core builds it from the same two inputs, so they agree to the token.
   */
  static async turnPrompt(ctx: common.Context, reading: boolean): Promise<string> {
    return yoai.nativePromptTurnSystem(await ChatSettings.stored(ctx), reading);
  }
}
