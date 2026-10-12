/**
 * The on-device language model a new user is offered to download.
 *
 * A new user with no key and no GGUF has nothing to chat with, and nothing
 * tells them which of the thousands of GGUFs on Hugging Face runs here. So the
 * settings screen offers one, beside the picker for a file they already have.
 * The download itself is download.ts's.
 */
import { join } from 'node:path'
import type { LocalOffer, ModelDownload } from '../shared/api'
import { Downloader, KnownFile } from './download'

/**
 * Qwen3.5 4B at Q4_K_M, 2.7 GB: small enough for a laptop's memory alongside
 * the speech models. Size and hash are the repository's own at this revision.
 */
export const OFFER: KnownFile = {
  name: 'Qwen3.5-4B-Q4_K_M.gguf',
  size: 2740937888,
  sha256: '00fe7986ff5f6b463e62455821146049db6f9313603938a70800d1fb69ef11a4',
  repo: 'unsloth/Qwen3.5-4B-GGUF',
  revision: 'e87f176479d0855a907a41277aca2f8ee7a09523'
}

export function offer(): LocalOffer {
  return { name: OFFER.name, size: OFFER.size }
}

const downloader = new Downloader('language model')

export function downloadStatus(): ModelDownload {
  return downloader.status()
}

export function cancelDownload(): void {
  downloader.cancel()
}

/**
 * Fetch the model into `dir`, unless it is already there, then hand its path to
 * `apply`. See Downloader.run.
 */
export function download(dir: string, apply: (path: string) => void): Promise<void> {
  return downloader.run(dir, [OFFER], () => apply(join(dir, OFFER.name)))
}
