/**
 * The system prompt for the spoken reply, editable on the device.
 *
 * On disk because prompt work is iterative and a rebuild per word is not
 * iteration. It is also the setting most likely to be wrong: everything else
 * in this lane is measurable from a log, and whether a reply sounds like a
 * person cannot be.
 */
import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';

const STORE_NAME: string = 'voice';
const KEY_PROMPT: string = 'chainPrompt';

/**
 * Shown, not described.
 *
 * The version before this one said "不要复述或概括他说的内容" and made restating
 * WORSE. Negation is weak instruction for a small model -- it reads the
 * content words and not the "不要", so naming the unwanted behaviour raises it
 * -- and "接一句相关的" actively invites hanging the reply on the original
 * words. Three examples demonstrate the shape instead, with no prohibition
 * anywhere in the text.
 *
 * Examples are affordable because of the prefix cache in llm/chat/local.cpp: this
 * block is prefilled once per session and reused by every turn after it.
 */
export const DEFAULT_CHAIN_PROMPT: string =
  '对方在口述刚发生的事。你像朋友一样随口接一句。\n' +
  '\n' +
  '他说「今天中午吃了宫保鸡丁，辣得我出汗」\n' +
  '你说「够劲儿，下午还撑得住吗」\n' +
  '\n' +
  '他说「昨晚没睡好，三点才睡着」\n' +
  '你说「那今天难熬了，中午补一觉吧」\n' +
  '\n' +
  '他说「下午去跑了五公里」\n' +
  '你说「这天儿能跑五公里挺厉害」\n' +
  '\n' +
  '只说一句，二十字以内。';

export class ChainPrompt {
  private static store: preferences.Preferences | null = null;

  private static async prefs(ctx: common.UIAbilityContext): Promise<preferences.Preferences> {
    if (ChainPrompt.store === null) {
      ChainPrompt.store = await preferences.getPreferences(ctx, STORE_NAME);
    }
    return ChainPrompt.store;
  }

  static async get(ctx: common.UIAbilityContext): Promise<string> {
    const p: preferences.Preferences = await ChainPrompt.prefs(ctx);
    const saved: string = await p.get(KEY_PROMPT, '') as string;
    return saved.length > 0 ? saved : DEFAULT_CHAIN_PROMPT;
  }

  /** An empty string restores the default rather than sending no prompt at all. */
  static async set(ctx: common.UIAbilityContext, text: string): Promise<void> {
    const p: preferences.Preferences = await ChainPrompt.prefs(ctx);
    await p.put(KEY_PROMPT, text.trim());
    await p.flush();
  }
}
