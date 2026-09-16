/**
 * The two axes that are read from WORDS rather than from sound.
 *
 * design/voice.md splits the axes by channel, and that split decides what has to
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
import bodyclock from 'libbodyclock.so';

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

/**
 * A five-point scale, enumerated as literals.
 *
 * GBNF cannot state a numeric range, and it does not need to here: five points
 * is already finer than a model can reliably distinguish on this judgment, and
 * enumerating them makes every reply both in range and parseable by
 * construction. Whitespace is not permitted either -- there is exactly one
 * string the model is allowed to emit for any given pair.
 */
const GRAMMAR: string =
  'root ::= "{\\"valence\\":" score ",\\"dominance\\":" score "}"\n' +
  'score ::= "-1" | "-0.5" | "0" | "0.5" | "1"\n';

/**
 * Definitions lifted from design/voice.md rather than reinvented, because the
 * dominance one is easy to get wrong: it is about agency, not sentiment, and the
 * same words can be identical in sentiment while the agency is reversed.
 */
const SYSTEM: string =
  'You judge two things about a short spoken note, from its words alone.\n\n' +
  'valence: pleasant to unpleasant. Is what they describe good or bad?\n' +
  '  -1 very unpleasant, 0 neutral, +1 very pleasant.\n\n' +
  'dominance: who is the subject of the verbs. Not sentiment — agency.\n' +
  '  "it got away from me", "I had to", "I could not", 「只能」,「没办法」\n' +
  '  put the speaker on the receiving end: negative.\n' +
  '  "I stopped at four", "I decided to" put them at the wheel: positive.\n' +
  '  -1 acted upon, 0 neither, +1 in control.\n\n' +
  'Answer with the JSON object and nothing else.';

export class TextAxesReader {
  /** Exposed so a debug screen can show what the reply was constrained to. */
  static grammar(): string {
    return GRAMMAR;
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
      if (providerId.length === 0 || transcript.trim().length === 0) {
        resolve({ axes: undefined, provider: providerId, sent: transcript, raw: '' });
        return;
      }
      const wire: string = JSON.stringify([{ role: 'user', content: transcript }]);
      let acc: string = '';

      bodyclock.nativeChat(providerId, wire, SYSTEM, GRAMMAR,
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
