/**
 * The two axes that are read from WORDS rather than from sound.
 *
 * docs/voice.md splits the axes by channel, and that split decides what has to
 * be built:
 *
 *   - arousal is acoustic, and prosody.cpp measures it with no model at all;
 *   - **valence is semantic** — "today was awful" states it outright;
 *   - **dominance is grammatical** — not which words are chosen but who is the
 *     subject of the verbs.
 *
 * SenseVoice produces a transcript but does not read it: it is a recogniser, not
 * a language model. So these two need the chat model the user has already
 * configured, which is what the design means by calling them "free" — that model
 * is going to read the transcript anyway.
 *
 * The point of having them is not completeness. It is the DISAGREEMENT: cheerful
 * words in a tired voice is the case only two channels can show, and neither
 * column is wrong when they differ. They are answering different questions.
 */
import yoai from 'libyoai.so';

export interface TextAxes {
  valence: number;     // -1 unpleasant .. +1 pleasant
  dominance: number;   // -1 acted upon .. +1 at the wheel
}

/**
 * The answer plus what produced it.
 *
 * A debug screen showing only the verdict cannot be debugged: when a cheerful
 * note comes back at -1 there is no way to tell whether the model misjudged it,
 * a different model answered than the one you think is configured, or the
 * transcript that was actually sent is not what was said. All three look
 * identical from the number alone, so all three inputs are carried out.
 */
export interface TextAxesResult {
  axes: TextAxes | undefined;
  provider: string;    // which registered model answered
  sent: string;        // the transcript as the model received it
  raw: string;         // every 'reply' byte, before parsing
}

/** One entry of nativePrompts(), the fields read here. */
interface PromptJson {
  name: string;
  text: string;
  grammar: string;
}

/**
 * The scoring prompt and its grammar, from the core (src/llm/prompt/prompts.cpp,
 * `text_axes`), where the reasons for both are written down: the definitions
 * follow docs/voice.md because the dominance one is easy to get wrong, and the
 * grammar enumerates a five-point scale so every reply is in range and
 * parseable by construction. This app kept its own copy until the core did.
 */
function textAxesPrompt(): PromptJson | undefined {
  try {
    const all: PromptJson[] = JSON.parse(yoai.nativePrompts()) as PromptJson[];
    for (const p of all) {
      if (p.name === 'text_axes') {
        return p;
      }
    }
  } catch (err) {
    // A core without the table: the caller reports no answer.
  }
  return undefined;
}

export class TextAxesReader {
  /** Exposed so a debug screen can show what the reply was constrained to. */
  static grammar(): string {
    const p: PromptJson | undefined = textAxesPrompt();
    return p === undefined ? '' : p.grammar;
  }

  /**
   * Read both axes off one transcript. Resolves undefined when there is no
   * usable answer -- no model configured, the turn failed, or a reply that did
   * not parse.
   *
   * A remote provider CANNOT be constrained (GBNF is a llama.cpp concept), so
   * its reply is ordinary text that usually but not always obeys the prompt.
   * That is why the parse below is defensive even though the grammar exists.
   */
  static read(providerId: string, transcript: string): Promise<TextAxesResult> {
    return new Promise<TextAxesResult>((resolve: (v: TextAxesResult) => void) => {
      const prompt: PromptJson | undefined = textAxesPrompt();
      if (providerId.length === 0 || transcript.trim().length === 0 || prompt === undefined) {
        resolve({ axes: undefined, provider: providerId, sent: transcript, raw: '' });
        return;
      }
      const wire: string = JSON.stringify([{ role: 'user', content: transcript }]);
      let acc: string = '';

      yoai.nativeChat(providerId, wire, prompt.text, prompt.grammar,
        (type: string, content: string) => {
          if (type === 'reply') {
            acc += content;
          } else if (type === 'error') {
            // Kept in `raw` rather than discarded: the provider's own words are
            // usually the whole explanation.
            resolve({ axes: undefined, provider: providerId, sent: transcript,
                      raw: 'error: ' + content });
          } else if (type === 'end') {
            resolve({ axes: TextAxesReader.parse(acc), provider: providerId,
                      sent: transcript, raw: acc });
          }
        });
    });
  }

  private static parse(raw: string): TextAxes | undefined {
    // An unconstrained remote reply may carry prose around the object, so the
    // braces are located rather than assumed to be the whole string.
    const open: number = raw.indexOf('{');
    const close: number = raw.lastIndexOf('}');
    if (open < 0 || close <= open) {
      return undefined;
    }
    try {
      const parsed: TextAxes = JSON.parse(raw.substring(open, close + 1)) as TextAxes;
      if (typeof parsed.valence !== 'number' || typeof parsed.dominance !== 'number') {
        return undefined;
      }
      return parsed;
    } catch (err) {
      return undefined;
    }
  }
}
