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
 * Nothing goes under userData. The files are 1.5 GB, userData on Windows is the
 * roaming %APPDATA%, and a folder the user picked is one they know to clean up.
 * As with every other model file here, the app references the files where they
 * are and never moves or deletes a finished one.
 */
import { net } from 'electron'
import { createHash } from 'node:crypto'
import { createReadStream, readdirSync, statSync } from 'node:fs'
import { open, rename, rm, statfs } from 'node:fs/promises'
import { basename, join } from 'node:path'
import { setTimeout as sleep } from 'node:timers/promises'
import type { TtsDownload, TtsSlot } from '../shared/api'

/** A file this app knows by name, size and SHA-256. */
interface KnownFile {
  name: string
  size: number
  sha256: string
}

/**
 * What a download fetches for each slot.
 *
 * 1.7B at Q4_K_M for both talkers: the Base one is what the desktop reads with
 * in design/voice.md, and the 0.6B Base at Q4_K_M misreads characters there
 * (调暗 as diào). The tokenizer at Q8_0, the one tests/tts_rtf.py loads. Sizes
 * and hashes are the repository's own at REVISION, and match the files these
 * measurements were taken with.
 */
export const KNOWN: Record<TtsSlot, KnownFile> = {
  talker: {
    name: 'qwen-talker-1.7b-base-Q4_K_M.gguf',
    size: 1219245248,
    sha256: 'ea393ebaf2167ea23ce9fc18b093822851358a950d7075cd47ab4f6ce23e887d'
  },
  customTalker: {
    name: 'qwen-talker-1.7b-customvoice-Q4_K_M.gguf',
    size: 1182631296,
    sha256: 'cc328834a631bc08bf9f43e62fa23f8a1383d9b429864ce6690cfb172077fc4a'
  },
  tokenizer: {
    name: 'qwen-tokenizer-12hz-Q8_0.gguf',
    size: 291150624,
    sha256: '1883beeed99348fc35e23dd225e9082f93f6f8c109330a33d935baa8acdbfd94'
  }
}

/**
 * A commit, not `main`: a file replaced upstream would fail the hash check, and
 * the user could do nothing about it.
 */
const REPO = 'Serveurperso/Qwen3-TTS-GGUF'
const REVISION = 'b7ee2e8c7459c3bea99da23e3d178125a7d1713c'

/**
 * Both asked at once, and the first to answer is used. huggingface.co cannot be
 * reached from mainland China, where hf-mirror.com serves the same paths; tried
 * from outside, hf-mirror.com redirected to huggingface.co. So nobody has to say
 * where they are.
 */
const HOSTS = ['https://huggingface.co', 'https://hf-mirror.com']

/** How long a host has to answer before it is given up on. */
const PROBE_MS = 15000

/** How long a transfer may go without a byte before it counts as broken off. */
const STALL_MS = 30000

/**
 * The waits before each retry after the connection breaks, three in all. The
 * count starts over once a retry has brought bytes in: that is a new break, not
 * the same one again, and a long download over a flaky line should survive
 * several of them.
 */
const RETRY_DELAYS_MS = [5000, 15000, 30000]

function urlOf(host: string, name: string): string {
  return `${host}/${REPO}/resolve/${REVISION}/${name}`
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

/** A download failure, with the code TtsDownload.error reports. */
class DownloadError extends Error {
  constructor(
    readonly code: Exclude<TtsDownload['error'], ''>,
    detail: string
  ) {
    super(detail)
  }
}

const IDLE: TtsDownload = {
  running: false,
  dir: '',
  file: '',
  host: '',
  received: 0,
  total: 0,
  verifying: false,
  retry: 0,
  retries: RETRY_DELAYS_MS.length,
  retryIn: 0,
  error: '',
  detail: ''
}

/** The one download there can be, read by the settings screen as it polls. */
let state: TtsDownload = { ...IDLE }
let controller: AbortController | null = null
/** When the pending retry starts, in Date.now() terms; 0 when none is pending. */
let retryAt = 0

export function downloadStatus(): TtsDownload {
  return {
    ...state,
    retryIn: state.retry > 0 ? Math.max(0, Math.ceil((retryAt - Date.now()) / 1000)) : 0
  }
}

export function cancelDownload(): void {
  controller?.abort()
}

/** -1 when there is nothing at `path`. */
function sizeOf(path: string): number {
  try {
    return statSync(path).size
  } catch {
    return -1
  }
}

/**
 * Fetch into `dir` whichever of `slots` is not already there, then hand every
 * slot's path to `apply`. Resolves when it is over; downloadStatus() says how
 * it went.
 *
 * `apply` runs before `running` goes false, so a screen that sees the download
 * finished also sees the paths it stored.
 *
 * A file is fetched as NAME.part and renamed only after its hash matches, so a
 * file under the real name is always whole. A transfer that breaks off is
 * retried after RETRY_DELAYS_MS, resuming the .part with a Range request; when
 * the retries run out the .part stays, and the next download resumes it. A
 * cancel deletes it: being asked to stop means not leaving a gigabyte behind.
 */
export async function download(
  dir: string,
  slots: TtsSlot[],
  apply: (paths: Partial<Record<TtsSlot, string>>) => void
): Promise<void> {
  if (state.running) {
    return
  }
  const abort = new AbortController()
  controller = abort
  state = { ...IDLE, running: true, dir }
  let part = ''

  try {
    const paths: Partial<Record<TtsSlot, string>> = {}
    const todo: KnownFile[] = []
    for (const slot of slots) {
      const path = join(dir, KNOWN[slot].name)
      // Already there, by name and size. Not hashed: a file the user put there
      // is theirs to vouch for, and the load says if it is not a model.
      if (sizeOf(path) === KNOWN[slot].size) {
        paths[slot] = path
      } else {
        todo.push(KNOWN[slot])
      }
    }

    if (todo.length > 0) {
      state.total = todo.reduce((sum, f) => sum + f.size, 0)
      // What a resume keeps. A .part longer than its file is none of it:
      // fetchOne() starts that one again.
      state.received = todo.reduce((sum, f) => {
        const got = sizeOf(join(dir, `${f.name}.part`))
        return sum + (got > 0 && got <= f.size ? got : 0)
      }, 0)
      const free = await freeBytes(dir)
      const need = state.total - state.received
      if (free !== null && free < need) {
        const gb = (n: number): string => `${(n / 1e9).toFixed(2)} GB`
        throw new DownloadError('space', `${gb(need)} needed, ${gb(free)} free`)
      }

      // From the first file not yet whole. The host is asked for again on
      // every attempt: the one that broke off may not be the one that answers.
      let next = 0
      const fetchRest = async (): Promise<void> => {
        const host = await pickHost(todo[next].name, abort.signal)
        state.host = new URL(host).host
        for (; next < todo.length; next++) {
          const file = todo[next]
          state.file = file.name
          part = join(dir, `${file.name}.part`)
          await fetchOne(host, part, file, abort.signal)
          await rename(part, join(dir, file.name))
          part = ''
        }
      }

      let attempt = 0
      let failedAt = -1
      for (;;) {
        try {
          await fetchRest()
          break
        } catch (err) {
          if (abort.signal.aborted || !(err instanceof DownloadError) || err.code !== 'network') {
            throw err
          }
          if (state.received > failedAt) {
            attempt = 0
          }
          if (attempt >= RETRY_DELAYS_MS.length) {
            throw err
          }
          failedAt = state.received
          console.warn(`[yoai] the speech model download broke off, retrying: ${err.message}`)
          state.retry = attempt + 1
          retryAt = Date.now() + RETRY_DELAYS_MS[attempt]
          try {
            await sleep(RETRY_DELAYS_MS[attempt], undefined, { signal: abort.signal })
          } finally {
            state.retry = 0
            retryAt = 0
          }
          attempt++
        }
      }
      for (const slot of slots) {
        paths[slot] = join(dir, KNOWN[slot].name)
      }
    }

    apply(paths)
  } catch (err) {
    if (abort.signal.aborted) {
      state.error = 'canceled'
      state.detail = ''
      if (part.length > 0) {
        await rm(part, { force: true }).catch(() => undefined)
      }
    } else if (err instanceof DownloadError) {
      state.error = err.code
      state.detail = err.message
    } else {
      // From the file system -- opening, writing, renaming -- since every
      // network failure is wrapped where it happens.
      state.error = 'disk'
      state.detail = err instanceof Error ? err.message : String(err)
    }
    console.error(`[yoai] the speech model download stopped: ${state.error} ${state.detail}`)
  } finally {
    state.running = false
    state.verifying = false
    state.retry = 0
    retryAt = 0
    controller = null
  }
}

/** Free bytes on the volume holding `dir`; null when the system will not say. */
async function freeBytes(dir: string): Promise<number | null> {
  try {
    const s = await statfs(dir)
    return s.bavail * s.bsize
  } catch {
    return null
  }
}

/**
 * The first host to answer a HEAD for `name`, redirects followed. The others are
 * stopped once one has answered.
 */
async function pickHost(name: string, signal: AbortSignal): Promise<string> {
  const losers = new AbortController()
  const ask = async (host: string): Promise<string> => {
    const res = await net.fetch(urlOf(host, name), {
      method: 'HEAD',
      signal: AbortSignal.any([signal, losers.signal, AbortSignal.timeout(PROBE_MS)])
    })
    if (!res.ok) {
      throw new Error(`${new URL(host).host}: HTTP ${res.status}`)
    }
    return host
  }
  try {
    return await Promise.any(HOSTS.map(ask))
  } catch (err) {
    const reasons =
      err instanceof AggregateError
        ? err.errors.map((e) => (e instanceof Error ? e.message : String(e)))
        : [String(err)]
    throw new DownloadError('network', reasons.join('; '))
  } finally {
    losers.abort()
  }
}

/**
 * Bring `part` up to the whole of `file` and check its hash. Resumes from what
 * is already in `part`; a server that ignores the Range header gets the file
 * written again from the start.
 */
async function fetchOne(host: string, part: string, file: KnownFile, signal: AbortSignal): Promise<void> {
  let start = sizeOf(part)
  if (start > file.size) {
    await rm(part, { force: true })
    start = 0
  }
  start = Math.max(start, 0)

  if (start < file.size) {
    // A connection that goes quiet never fails on its own, so without this a
    // stalled transfer would wait forever and never reach a retry.
    const stall = new AbortController()
    let timer = setTimeout(() => stall.abort(), STALL_MS)
    const alive = (): void => {
      clearTimeout(timer)
      timer = setTimeout(() => stall.abort(), STALL_MS)
    }
    const broke = (err: unknown): DownloadError =>
      new DownloadError(
        'network',
        stall.signal.aborted
          ? `${file.name}: nothing arrived for ${STALL_MS / 1000}s`
          : err instanceof Error
            ? err.message
            : String(err)
      )

    try {
      let res: Response
      try {
        res = await net.fetch(urlOf(host, file.name), {
          headers: start > 0 ? { Range: `bytes=${start}-` } : {},
          signal: AbortSignal.any([signal, stall.signal])
        })
      } catch (err) {
        throw broke(err)
      }
      if (res.status === 200 && start > 0) {
        state.received -= start
        start = 0
      } else if (res.status !== 200 && res.status !== 206) {
        throw new DownloadError('network', `${file.name}: HTTP ${res.status}`)
      }
      if (res.body === null) {
        throw new DownloadError('network', `${file.name}: an empty response`)
      }

      const out = await open(part, start > 0 ? 'a' : 'w')
      try {
        const reader = res.body.getReader()
        for (;;) {
          const chunk = await reader.read().catch((err: unknown) => {
            throw broke(err)
          })
          if (chunk.done) {
            break
          }
          alive()
          await out.write(chunk.value)
          state.received += chunk.value.length
        }
      } finally {
        await out.close()
      }
    } finally {
      clearTimeout(timer)
    }
  }

  const got = sizeOf(part)
  if (got !== file.size) {
    throw new DownloadError('network', `${file.name}: ${got} of ${file.size} bytes arrived`)
  }

  state.verifying = true
  const sum = await sha256Of(part, signal)
  state.verifying = false
  if (sum !== file.sha256) {
    // Deleted rather than kept for a resume: resuming would keep the bad bytes.
    await rm(part, { force: true })
    throw new DownloadError('checksum', `${file.name}: sha256 ${sum}`)
  }
}

async function sha256Of(path: string, signal: AbortSignal): Promise<string> {
  const hash = createHash('sha256')
  const stream = createReadStream(path, { highWaterMark: 4 * 1024 * 1024 })
  try {
    for await (const chunk of stream) {
      if (signal.aborted) {
        throw new Error('canceled')
      }
      hash.update(chunk as Buffer)
    }
  } finally {
    stream.destroy()
  }
  return hash.digest('hex')
}
