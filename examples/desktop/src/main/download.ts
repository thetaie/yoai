/**
 * Model files fetched from Hugging Face into a folder the user chose.
 *
 * One engine for every model the app offers to download -- the speech models
 * in ttsfiles.ts, the language model in llmfiles.ts. Each holds a Downloader of
 * its own, so the two can run side by side and each settings card follows its
 * own.
 *
 * Nothing goes under userData. The files are gigabytes, userData on Windows is
 * the roaming %APPDATA%, and a folder the user picked is one they know to clean
 * up. As with every other model file here, the app references the files where
 * they are and never moves or deletes a finished one.
 */
import { net } from 'electron'
import { createHash } from 'node:crypto'
import { createReadStream, statSync } from 'node:fs'
import { open, rename, rm, statfs } from 'node:fs/promises'
import { join } from 'node:path'
import { setTimeout as sleep } from 'node:timers/promises'
import type { ModelDownload } from '../shared/api'

/**
 * A file this app knows by name, size and SHA-256, and where it is published.
 *
 * At a commit, not `main`: a file replaced upstream would fail the hash check,
 * and the user could do nothing about it.
 */
export interface KnownFile {
  name: string
  size: number
  sha256: string
  /** The Hugging Face repository, owner/name. */
  repo: string
  revision: string
}

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

function urlOf(host: string, file: KnownFile): string {
  return `${host}/${file.repo}/resolve/${file.revision}/${file.name}`
}

/** -1 when there is nothing at `path`. */
export function sizeOf(path: string): number {
  try {
    return statSync(path).size
  } catch {
    return -1
  }
}

/** A download failure, with the code ModelDownload.error reports. */
class DownloadError extends Error {
  constructor(
    readonly code: Exclude<ModelDownload['error'], ''>,
    detail: string
  ) {
    super(detail)
  }
}

const IDLE: ModelDownload = {
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

/** One download at a time, read by a settings card as it polls. */
export class Downloader {
  private state: ModelDownload = { ...IDLE }
  private controller: AbortController | null = null
  /** When the pending retry starts, in Date.now() terms; 0 when none is pending. */
  private retryAt = 0

  /** `what` names the files in the log: "the speech model". */
  constructor(private readonly what: string) {}

  status(): ModelDownload {
    return {
      ...this.state,
      retryIn:
        this.state.retry > 0 ? Math.max(0, Math.ceil((this.retryAt - Date.now()) / 1000)) : 0
    }
  }

  cancel(): void {
    this.controller?.abort()
  }

  /**
   * Fetch into `dir` whichever of `files` is not already there, then call
   * `done`. Resolves when it is over; status() says how it went.
   *
   * `done` runs before `running` goes false, so a screen that sees the download
   * finished also sees what it stored.
   *
   * A file is fetched as NAME.part and renamed only after its hash matches, so
   * a file under the real name is always whole. A transfer that breaks off is
   * retried after RETRY_DELAYS_MS, resuming the .part with a Range request;
   * when the retries run out the .part stays, and the next download resumes it.
   * A cancel deletes it: being asked to stop means not leaving a gigabyte
   * behind.
   */
  async run(dir: string, files: KnownFile[], done: () => void): Promise<void> {
    if (this.state.running) {
      return
    }
    const abort = new AbortController()
    this.controller = abort
    this.state = { ...IDLE, running: true, dir }
    const state = this.state
    let part = ''

    try {
      // Already there, by name and size. Not hashed: a file the user put there
      // is theirs to vouch for, and the load says if it is not a model.
      const todo = files.filter((f) => sizeOf(join(dir, f.name)) !== f.size)

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
          const host = await pickHost(todo[next], abort.signal)
          state.host = new URL(host).host
          for (; next < todo.length; next++) {
            const file = todo[next]
            state.file = file.name
            part = join(dir, `${file.name}.part`)
            await this.fetchOne(host, part, file, abort.signal)
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
            console.warn(`[yoai] the ${this.what} download broke off, retrying: ${err.message}`)
            state.retry = attempt + 1
            this.retryAt = Date.now() + RETRY_DELAYS_MS[attempt]
            try {
              await sleep(RETRY_DELAYS_MS[attempt], undefined, { signal: abort.signal })
            } finally {
              state.retry = 0
              this.retryAt = 0
            }
            attempt++
          }
        }
      }

      done()
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
      console.error(`[yoai] the ${this.what} download stopped: ${state.error} ${state.detail}`)
    } finally {
      state.running = false
      state.verifying = false
      state.retry = 0
      this.retryAt = 0
      this.controller = null
    }
  }

  /**
   * Bring `part` up to the whole of `file` and check its hash. Resumes from what
   * is already in `part`; a server that ignores the Range header gets the file
   * written again from the start.
   */
  private async fetchOne(
    host: string,
    part: string,
    file: KnownFile,
    signal: AbortSignal
  ): Promise<void> {
    const state = this.state
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
          res = await net.fetch(urlOf(host, file), {
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
 * The first host to answer a HEAD for `file`, redirects followed. The others are
 * stopped once one has answered.
 */
async function pickHost(file: KnownFile, signal: AbortSignal): Promise<string> {
  const losers = new AbortController()
  const ask = async (host: string): Promise<string> => {
    const res = await net.fetch(urlOf(host, file), {
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
