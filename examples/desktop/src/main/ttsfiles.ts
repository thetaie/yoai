/**
 * The speech models' files: found beside one the user picked, or downloaded
 * into a folder the user chose.
 *
 * A new user has neither file, and nothing tells them which two of the upstream
 * repository's two dozen to fetch. A returning one may have them already,
 * somewhere this app cannot guess. So it does not guess: the settings screen
 * offers both ways, and each one survives being the wrong choice. Downloading
 * into a folder that already holds the files fetches nothing, and picking one
 * file fills the other from the same folder.
 *
 * The download itself is download.ts's.
 */
import { readdirSync } from 'node:fs'
import { basename, join } from 'node:path'
import type { ModelDownload, TtsSlot } from '../shared/api'
import { Downloader, KnownFile } from './download'

/**
 * A commit, not `main`; see KnownFile. Sizes and hashes are the repository's
 * own at this revision, and match the files these measurements were taken with.
 */
const REPO = 'Serveurperso/Qwen3-TTS-GGUF'
const REVISION = 'b7ee2e8c7459c3bea99da23e3d178125a7d1713c'

/**
 * What a download fetches for each slot.
 *
 * 1.7B at Q4_K_M for both talkers: the Base one is what the desktop reads with
 * in docs/voice.md, and the 0.6B Base at Q4_K_M misreads characters there
 * (调暗 as diào). The tokenizer at Q8_0, the one tests/tts_rtf.py loads.
 */
export const KNOWN: Record<TtsSlot, KnownFile> = {
  talker: {
    name: 'qwen-talker-1.7b-base-Q4_K_M.gguf',
    size: 1219245248,
    sha256: 'ea393ebaf2167ea23ce9fc18b093822851358a950d7075cd47ab4f6ce23e887d',
    repo: REPO,
    revision: REVISION
  },
  customTalker: {
    name: 'qwen-talker-1.7b-customvoice-Q4_K_M.gguf',
    size: 1182631296,
    sha256: 'cc328834a631bc08bf9f43e62fa23f8a1383d9b429864ce6690cfb172077fc4a',
    repo: REPO,
    revision: REVISION
  },
  tokenizer: {
    name: 'qwen-tokenizer-12hz-Q8_0.gguf',
    size: 291150624,
    sha256: '1883beeed99348fc35e23dd225e9082f93f6f8c109330a33d935baa8acdbfd94',
    repo: REPO,
    revision: REVISION
  }
}

/**
 * Which slot a file belongs in, by its name; null when the name does not say.
 *
 * By the words in the name rather than an exact pattern, so the upstream
 * naming (qwen-talker-1.7b-base-Q4_K_M.gguf) and the ggml-org one
 * (Qwen3-TTS-12Hz-1.7B-Base-Q4_K_M.gguf) both fit. "talker" or "tts" is
 * required for a talker, because the folder may hold language models too, and
 * "Qwen3.5-2B-Base" would otherwise read as a Base talker. A wrong guess
 * costs little: the load that follows names the file it refused.
 */
export function slotOf(path: string): TtsSlot | null {
  const n = basename(path).toLowerCase()
  if (!n.endsWith('.gguf')) {
    return null
  }
  if (n.includes('tokenizer') && n.includes('12hz')) {
    return 'tokenizer'
  }
  if (!n.includes('talker') && !n.includes('tts')) {
    return null
  }
  if (/custom-?voice/.test(n)) {
    return 'customTalker'
  }
  return n.includes('base') ? 'talker' : null
}

/**
 * A file in `dir` for each of `slots`: the one a download would fetch when it
 * is there, else the first by name that slotOf() places in that slot. Slots
 * with no such file are left out.
 */
export function findIn(dir: string, slots: TtsSlot[]): Partial<Record<TtsSlot, string>> {
  let names: string[]
  try {
    names = readdirSync(dir).sort()
  } catch {
    return {}
  }
  const found: Partial<Record<TtsSlot, string>> = {}
  for (const slot of slots) {
    const match = names.includes(KNOWN[slot].name)
      ? KNOWN[slot].name
      : names.find((n) => slotOf(n) === slot)
    if (match !== undefined) {
      found[slot] = join(dir, match)
    }
  }
  return found
}

const downloader = new Downloader('speech model')

export function downloadStatus(): ModelDownload {
  return downloader.status()
}

export function cancelDownload(): void {
  downloader.cancel()
}

/**
 * Fetch into `dir` whichever of `slots` is not already there, then hand every
 * slot's path to `apply`. See Downloader.run.
 */
export function download(
  dir: string,
  slots: TtsSlot[],
  apply: (paths: Partial<Record<TtsSlot, string>>) => void
): Promise<void> {
  return downloader.run(
    dir,
    slots.map((slot) => KNOWN[slot]),
    () => {
      const paths: Partial<Record<TtsSlot, string>> = {}
      for (const slot of slots) {
        paths[slot] = join(dir, KNOWN[slot].name)
      }
      apply(paths)
    }
  )
}
