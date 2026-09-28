/**
 * The system prompt a turn is sent with, built the same way wherever it is
 * needed: by the page when it sends the turn, and by the main process when it
 * computes the prompt's front ahead of that turn (the local model's warm-up).
 * The two have to agree to the token, or the warm-up is spent on a prompt
 * nobody sends.
 */

/**
 * What a reply that will be read aloud is asked to be, added to the system
 * prompt for that turn only.
 *
 * Written for the ear rather than the eye. The core strips markdown before the
 * engine sees it (src/voice/text/speakable.hpp), but a stripped list is still a
 * list: items with no connective between them, read one after another at the
 * same pace, which is most of what "mechanical" meant. Short sentences joined by
 * spoken connectives give the engine something to phrase. Numbers and units
 * written as they are said keep it from guessing how "3.5km" is read.
 *
 * Nothing here relies on how the engine treats fillers such as "嗯" -- that has
 * not been listened to yet, apart from "。。", which is heard as a hesitation.
 *
 * **One paragraph, no example connectives, and told not to mention itself.**
 * The first version was a bulleted list naming "其实", "然后", "不过" and
 * "第一……第二……", and the 4B model did two things with it: it started nearly
 * every sentence with one of those words, in that order, and asked who it was
 * it recited the list back. Both went away in this form, over four runs each.
 */
export const SPOKEN_STYLE =
  '这次的回答会被朗读出来，所以写成说话的样子：句子短，一句一件事，像平时聊天一样自然衔接，' +
  '连接词偶尔用一下就好，不要每句都用；不用列表、表格、标题、代码块和括号，要列举就连成句子说出来；' +
  '数字、单位和符号写成读法，比如“百分之二十”“三点五公里”；不用表情符号；简短，先说结论。' +
  '这是写法要求，不要在回答里提到。'

/**
 * The user's system prompt, and SPOKEN_STYLE after it when the reply will be
 * heard. Read on screen, lists and tables are the better form, so only then.
 */
export function turnSystemPrompt(base: string, reading: boolean): string {
  if (!reading) {
    return base
  }
  return [base, SPOKEN_STYLE].filter((p) => p.trim().length > 0).join('\n\n')
}
