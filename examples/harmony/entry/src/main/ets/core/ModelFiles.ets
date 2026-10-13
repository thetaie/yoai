/**
 * The model files this app offers to download, and how a file's name says
 * which speech slot it fills.
 *
 * The desktop keeps the same table in examples/desktop/src/main/llmfiles.ts and
 * ttsfiles.ts. The speech files are the same three; the language model is not,
 * see LLM_OFFER.
 */
import { KnownFile } from './Download';

/**
 * Qwen3.5 2B at Q4_K_M, 1.28 GB, where the desktop offers the 4B.
 *
 * On a phone the writer is the bottleneck twice over: the reply is read as it
 * is written, and docs/voice.md measured the 2B at twice the 4B's speed on the
 * desktop, while the 4B there already decodes at half its bench speed. The 2B
 * is also the model the voice pages on this app were measured with. Size and
 * hash are the repository's own at this revision.
 */
export const LLM_OFFER: KnownFile = {
  name: 'Qwen3.5-2B-Q4_K_M.gguf',
  size: 1280835840,
  sha256: 'aaf42c8b7c3cab2bf3d69c355048d4a0ee9973d48f16c731c0520ee914699223',
  repo: 'unsloth/Qwen3.5-2B-GGUF',
  revision: 'f6d5376be1edb4d416d56da11e5397a961aca8ae'
};

/** Which stored path a speech file fills. */
export type SpeechSlot = 'talker' | 'customTalker' | 'tokenizer';

/** In the order a mode lists them: the Base talker, the CustomVoice talker, the tokenizer. */
export const SPEECH_SLOTS: SpeechSlot[] = ['talker', 'customTalker', 'tokenizer'];

/** A commit, not `main`; see KnownFile. */
const TTS_REPO: string = 'Serveurperso/Qwen3-TTS-GGUF';
const TTS_REVISION: string = 'b7ee2e8c7459c3bea99da23e3d178125a7d1713c';

/**
 * What a download fetches for each slot. 1.7B at Q4_K_M for both talkers: the
 * 0.6B Base at Q4_K_M misreads characters (调暗 as diào), and on this class of
 * phone it is only about a fifth faster (README.md, the RTF table). The
 * tokenizer at Q8_0.
 */
export function speechFile(slot: SpeechSlot): KnownFile {
  if (slot === 'talker') {
    return {
      name: 'qwen-talker-1.7b-base-Q4_K_M.gguf',
      size: 1219245248,
      sha256: 'ea393ebaf2167ea23ce9fc18b093822851358a950d7075cd47ab4f6ce23e887d',
      repo: TTS_REPO,
      revision: TTS_REVISION
    };
  }
  if (slot === 'customTalker') {
    return {
      name: 'qwen-talker-1.7b-customvoice-Q4_K_M.gguf',
      size: 1182631296,
      sha256: 'cc328834a631bc08bf9f43e62fa23f8a1383d9b429864ce6690cfb172077fc4a',
      repo: TTS_REPO,
      revision: TTS_REVISION
    };
  }
  return {
    name: 'qwen-tokenizer-12hz-Q8_0.gguf',
    size: 291150624,
    sha256: '1883beeed99348fc35e23dd225e9082f93f6f8c109330a33d935baa8acdbfd94',
    repo: TTS_REPO,
    revision: TTS_REVISION
  };
}

/**
 * Which slot a file belongs in, by its name; '' when the name does not say.
 *
 * By the words in the name rather than an exact pattern, so the upstream
 * naming (qwen-talker-1.7b-base-Q4_K_M.gguf) and the ggml-org one
 * (Qwen3-TTS-12Hz-1.7B-Base-Q4_K_M.gguf) both fit. "talker" or "tts" is
 * required for a talker, because a folder may hold language models too, and
 * "Qwen3.5-2B-Base" would otherwise read as a Base talker. A wrong guess costs
 * little: the load that follows names the file it refused.
 */
export function slotOf(name: string): string {
  const n: string = name.toLowerCase();
  if (!n.endsWith('.gguf')) {
    return '';
  }
  if (n.includes('tokenizer') && n.includes('12hz')) {
    return 'tokenizer';
  }
  if (!n.includes('talker') && !n.includes('tts')) {
    return '';
  }
  if (n.includes('customvoice') || n.includes('custom-voice')) {
    return 'customTalker';
  }
  return n.includes('base') ? 'talker' : '';
}
