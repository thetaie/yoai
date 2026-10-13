/**
 * Model files fetched from Hugging Face into a folder.
 *
 * A port of examples/desktop/src/main/download.ts, and the same promises: a
 * file is written as NAME.part and takes its real name only after its SHA-256
 * matches, so a file under the real name is always whole; a transfer that
 * breaks off, or goes 30s without a byte, is retried after 5, 15 and 30s, each
 * time resuming from the .part; the count starts over once a retry brings bytes
 * in; a cancel deletes the .part; huggingface.co and hf-mirror.com are asked at
 * once and the first to answer is used.
 *
 * Where the desktop uses Electron's fetch, this uses the platform's streaming
 * HTTP request, and resumes with its `resumeFrom` rather than a Range header:
 * a server that ignored the range would send the whole file again, and with
 * `resumeFrom` that is a failed request rather than a .part with the start of
 * the file appended to its middle.
 *
 * It runs in the app's process, so it runs while the app does. A phone that
 * moves the app to the background may suspend it, and the transfer then breaks
 * off and resumes like any other.
 */
import { http } from '@kit.NetworkKit';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { BusinessError } from '@kit.BasicServicesKit';
import fs from '@ohos.file.fs';
import statvfs from '@ohos.file.statvfs';
import hilog from '@ohos.hilog';

/**
 * A file this app knows by name, size and SHA-256, and where it is published.
 *
 * At a commit, not `main`: a file replaced upstream would fail the hash check,
 * and the user could do nothing about it.
 */
export interface KnownFile {
  name: string;
  size: number;
  sha256: string;
  /** The Hugging Face repository, owner/name. */
  repo: string;
  revision: string;
}

/**
 * Where a download stands, for a settings card that polls it.
 *
 * `error` is why the last one stopped, '' when it did not fail: 'space' (not
 * enough room), 'network' (no host answered, or the retries ran out --
 * downloading again resumes), 'checksum' (not the file expected; deleted),
 * 'disk' (the folder could not be written), 'canceled'.
 */
export interface DownloadState {
  running: boolean;
  dir: string;
  /** The file being fetched or checked now. */
  file: string;
  /** huggingface.co or hf-mirror.com, whichever answered first; '' until one has. */
  host: string;
  /** Bytes on disk so far and in all, across every file this download fetches. */
  received: number;
  total: number;
  /** Checking `file` against its SHA-256, which takes seconds. */
  verifying: boolean;
  /** Which retry comes next, 1-based, of `retries`, and in how many seconds; 0 otherwise. */
  retry: number;
  retries: number;
  retryIn: number;
  error: string;
  /** The underlying message behind `error`, in English; '' when there is none. */
  detail: string;
}

const HOSTS: string[] = ['https://huggingface.co', 'https://hf-mirror.com'];

/** How long a host has to answer before it is given up on. */
const PROBE_MS: number = 15000;

/** How long a transfer may go without a byte before it counts as broken off. */
const STALL_MS: number = 30000;

/**
 * The longest one request may run. The platform's own default is a minute,
 * and it bounds the whole transfer rather than a silence -- far too short for a
 * gigabyte. Silence is STALL_MS's job.
 */
const REQUEST_MS: number = 6 * 60 * 60 * 1000;

/** The waits before each retry after the connection breaks, three in all. */
const RETRY_DELAYS_MS: number[] = [5000, 15000, 30000];

/** Bytes per read while hashing. */
const HASH_CHUNK: number = 4 * 1024 * 1024;

function log(msg: string): void {
  hilog.info(0x0000, 'yoai', '%{public}s', msg);
}

function urlOf(host: string, file: KnownFile): string {
  return host + '/' + file.repo + '/resolve/' + file.revision + '/' + file.name;
}

/** Decimal units, the way model sizes are quoted: 1.3 GB, 291 MB. */
export function formatBytes(n: number): string {
  return n >= 1e9 ? (n / 1e9).toFixed(1) + ' GB' : Math.round(n / 1e6) + ' MB';
}

/** -1 when there is nothing at `path`. */
export function sizeOf(path: string): number {
  try {
    return fs.statSync(path).size;
  } catch (err) {
    return -1;
  }
}

/** A failure with the code DownloadState.error reports. */
class DownloadError extends Error {
  code: string;

  constructor(code: string, detail: string) {
    super(detail);
    this.code = code;
  }
}

function idle(): DownloadState {
  return {
    running: false, dir: '', file: '', host: '', received: 0, total: 0,
    verifying: false, retry: 0, retries: RETRY_DELAYS_MS.length, retryIn: 0,
    error: '', detail: ''
  };
}

function gb(n: number): string {
  return (n / 1e9).toFixed(2) + ' GB';
}

function messageOf(err: Object): string {
  const e = err as BusinessError;
  if (e.code !== undefined && e.message !== undefined) {
    return e.code + ' ' + e.message;
  }
  return String(err);
}

/** One download at a time, read by a settings card as it polls. */
export class Downloader {
  private state: DownloadState = idle();
  private canceled: boolean = false;
  /**
   * Ends whatever is in flight -- a host probe or a transfer -- as cancelled.
   * Destroying a request is not enough by itself: nothing promises that its
   * pending promise settles afterwards, and a download waiting on one would
   * show as running forever.
   */
  private abort: (() => void) | null = null;
  /** Wakes a retry wait early when the download is cancelled. */
  private wake: (() => void) | null = null;
  /** When the pending retry starts, in Date.now() terms; 0 when none is pending. */
  private retryAt: number = 0;
  /** `what` names the files in the log: "speech model". */
  private what: string;

  constructor(what: string) {
    this.what = what;
  }

  status(): DownloadState {
    const s: DownloadState = {
      running: this.state.running, dir: this.state.dir, file: this.state.file,
      host: this.state.host, received: this.state.received, total: this.state.total,
      verifying: this.state.verifying, retry: this.state.retry, retries: this.state.retries,
      retryIn: 0, error: this.state.error, detail: this.state.detail
    };
    if (s.retry > 0) {
      s.retryIn = Math.max(0, Math.ceil((this.retryAt - Date.now()) / 1000));
    }
    return s;
  }

  cancel(): void {
    if (!this.state.running) {
      return;
    }
    this.canceled = true;
    if (this.abort !== null) {
      this.abort();
    }
    if (this.wake !== null) {
      this.wake();
    }
  }

  /**
   * Fetch into `dir` whichever of `files` is not already there, then call
   * `done`. Resolves when it is over; status() says how it went.
   *
   * `done` runs before `running` goes false, so a screen that sees the download
   * finished also sees what it stored.
   */
  async run(dir: string, files: KnownFile[], done: () => Promise<void>): Promise<void> {
    if (this.state.running) {
      return;
    }
    this.canceled = false;
    this.state = idle();
    this.state.running = true;
    this.state.dir = dir;
    let part: string = '';

    try {
      // Already there, by name and size. Not hashed: a file the user put there
      // is theirs to vouch for, and the load says if it is not a model.
      const todo: KnownFile[] = [];
      for (const f of files) {
        if (sizeOf(dir + '/' + f.name) !== f.size) {
          todo.push(f);
        }
      }

      if (todo.length > 0) {
        let total: number = 0;
        let have: number = 0;
        for (const f of todo) {
          total += f.size;
          // What a resume keeps. A .part longer than its file is none of it.
          const got: number = sizeOf(dir + '/' + f.name + '.part');
          have += got > 0 && got <= f.size ? got : 0;
        }
        this.state.total = total;
        this.state.received = have;
        const free: number = await freeBytes(dir);
        if (free >= 0 && free < total - have) {
          throw new DownloadError('space', gb(total - have) + ' needed, ' + gb(free) + ' free');
        }

        // From the first file not yet whole. The host is asked for again on
        // every attempt: the one that broke off may not be the one that answers.
        let next: number = 0;
        let attempt: number = 0;
        let failedAt: number = -1;
        for (;;) {
          try {
            const host: string = await this.pickHost(todo[next]);
            this.state.host = host.substring(host.indexOf('//') + 2);
            for (; next < todo.length; next++) {
              if (this.canceled) {
                throw new DownloadError('canceled', '');
              }
              const file: KnownFile = todo[next];
              this.state.file = file.name;
              part = dir + '/' + file.name + '.part';
              await this.fetchOne(host, part, file);
              fs.renameSync(part, dir + '/' + file.name);
              part = '';
            }
            break;
          } catch (err) {
            if (this.canceled || !(err instanceof DownloadError) ||
                (err as DownloadError).code !== 'network') {
              throw err as Error;
            }
            if (this.state.received > failedAt) {
              attempt = 0;
            }
            if (attempt >= RETRY_DELAYS_MS.length) {
              throw err as Error;
            }
            failedAt = this.state.received;
            log('the ' + this.what + ' download broke off, retrying: ' + (err as DownloadError).message);
            this.state.retry = attempt + 1;
            this.retryAt = Date.now() + RETRY_DELAYS_MS[attempt];
            await this.sleep(RETRY_DELAYS_MS[attempt]);
            this.state.retry = 0;
            this.retryAt = 0;
            if (this.canceled) {
              throw new DownloadError('canceled', '');
            }
            attempt++;
          }
        }
      }

      await done();
    } catch (err) {
      if (this.canceled) {
        this.state.error = 'canceled';
        this.state.detail = '';
        // Being asked to stop means not leaving a gigabyte behind.
        if (part.length > 0) {
          try {
            fs.unlinkSync(part);
          } catch (e) {
            // Never created, or already gone.
          }
        }
      } else if (err instanceof DownloadError) {
        this.state.error = (err as DownloadError).code;
        this.state.detail = (err as DownloadError).message;
      } else {
        // From the file system -- opening, writing, renaming -- since every
        // network failure is wrapped where it happens.
        this.state.error = 'disk';
        this.state.detail = messageOf(err as Object);
      }
      hilog.error(0x0000, 'yoai', 'the %{public}s download stopped: %{public}s %{public}s',
        this.what, this.state.error, this.state.detail);
    } finally {
      this.state.running = false;
      this.state.verifying = false;
      this.state.retry = 0;
      this.retryAt = 0;
      this.abort = null;
      this.wake = null;
    }
  }

  /** Wait, or stop waiting the moment the download is cancelled. */
  private sleep(ms: number): Promise<void> {
    return new Promise<void>((resolve: () => void) => {
      const timer: number = setTimeout(() => {
        this.wake = null;
        resolve();
      }, ms);
      this.wake = () => {
        clearTimeout(timer);
        this.wake = null;
        resolve();
      };
    });
  }

  /**
   * The first host to answer a HEAD for `file`, redirects followed. A host that
   * answers late is ignored rather than stopped: a HEAD costs nothing to finish.
   */
  private pickHost(file: KnownFile): Promise<string> {
    return new Promise<string>((resolve: (h: string) => void, reject: (e: DownloadError) => void) => {
      let settled: boolean = false;
      const reasons: string[] = [];
      this.abort = () => {
        if (!settled) {
          settled = true;
          reject(new DownloadError('canceled', ''));
        }
      };
      for (const host of HOSTS) {
        const req: http.HttpRequest = http.createHttp();
        const opts: http.HttpRequestOptions = {
          method: http.RequestMethod.HEAD,
          connectTimeout: PROBE_MS,
          readTimeout: PROBE_MS,
          usingCache: false
        };
        req.request(urlOf(host, file), opts).then((res: http.HttpResponse) => {
          req.destroy();
          if (res.responseCode >= 200 && res.responseCode < 300) {
            if (!settled) {
              settled = true;
              resolve(host);
            }
            return;
          }
          reasons.push(host.substring(host.indexOf('//') + 2) + ': HTTP ' + res.responseCode);
          if (reasons.length === HOSTS.length && !settled) {
            settled = true;
            reject(new DownloadError('network', reasons.join('; ')));
          }
        }).catch((err: BusinessError) => {
          req.destroy();
          reasons.push(host.substring(host.indexOf('//') + 2) + ': ' + messageOf(err));
          if (reasons.length === HOSTS.length && !settled) {
            settled = true;
            reject(new DownloadError('network', reasons.join('; ')));
          }
        });
      }
    });
  }

  /**
   * Bring `part` up to the whole of `file` and check its hash, resuming from
   * what is already in `part`.
   */
  private async fetchOne(host: string, part: string, file: KnownFile): Promise<void> {
    let start: number = sizeOf(part);
    if (start > file.size) {
      fs.unlinkSync(part);
      start = 0;
    }
    start = Math.max(start, 0);

    if (start < file.size) {
      await this.transfer(host, part, file, start);
    }

    const got: number = sizeOf(part);
    if (got !== file.size) {
      throw new DownloadError('network', file.name + ': ' + got + ' of ' + file.size + ' bytes arrived');
    }

    this.state.verifying = true;
    const sum: string = await sha256Of(part, () => this.canceled);
    this.state.verifying = false;
    if (this.canceled) {
      throw new DownloadError('canceled', '');
    }
    if (sum !== file.sha256) {
      // Deleted rather than kept for a resume: resuming would keep the bad bytes.
      fs.unlinkSync(part);
      throw new DownloadError('checksum', file.name + ': sha256 ' + sum);
    }
  }

  /** One request: the bytes of `file` from `start` on, appended to `part`. */
  private transfer(host: string, part: string, file: KnownFile, start: number): Promise<void> {
    return new Promise<void>((resolve: () => void, reject: (e: DownloadError) => void) => {
      const out: fs.File = fs.openSync(part,
        fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | (start > 0 ? fs.OpenMode.APPEND : fs.OpenMode.TRUNC));
      const req: http.HttpRequest = http.createHttp();
      let over: boolean = false;

      // A connection that goes quiet never fails on its own, so without this a
      // stalled transfer would wait forever and never reach a retry.
      let timer: number = -1;
      const arm = (): void => {
        if (timer !== -1) {
          clearTimeout(timer);
        }
        timer = setTimeout(() => {
          finish(new DownloadError('network', file.name + ': nothing arrived for ' +
            (STALL_MS / 1000) + 's'));
        }, STALL_MS);
      };

      // The one way out, whichever of the request, the stall timer, a failed
      // write or a cancel gets there first.
      const finish = (err: DownloadError | null): void => {
        if (over) {
          return;
        }
        over = true;
        this.abort = null;
        if (timer !== -1) {
          clearTimeout(timer);
        }
        req.off('dataReceive');
        req.destroy();
        try {
          fs.closeSync(out);
        } catch (e) {
          // Closed already. Nothing to keep from it.
        }
        if (err !== null) {
          reject(err);
        } else {
          resolve();
        }
      };
      this.abort = () => {
        finish(new DownloadError('canceled', ''));
      };

      req.on('dataReceive', (chunk: ArrayBuffer) => {
        if (over) {
          return;
        }
        arm();
        try {
          fs.writeSync(out.fd, chunk);
          this.state.received += chunk.byteLength;
        } catch (e) {
          finish(new DownloadError('disk', messageOf(e as Object)));
        }
      });

      const opts: http.HttpRequestOptions = {
        method: http.RequestMethod.GET,
        connectTimeout: PROBE_MS,
        readTimeout: REQUEST_MS,
        usingCache: false
      };
      if (start > 0) {
        opts.resumeFrom = start;
      }
      arm();
      req.requestInStream(urlOf(host, file), opts).then((code: number) => {
        if (code !== 200 && code !== 206) {
          finish(new DownloadError('network', file.name + ': HTTP ' + code));
          return;
        }
        finish(null);
      }).catch((err: BusinessError) => {
        // A no-op when the stall timer, a write or a cancel ended it first.
        finish(new DownloadError('network', file.name + ': ' + messageOf(err)));
      });
    });
  }
}

/** Free bytes on the volume holding `dir`; -1 when the system will not say. */
async function freeBytes(dir: string): Promise<number> {
  try {
    return await statvfs.getFreeSize(dir);
  } catch (err) {
    return -1;
  }
}

/** Hex SHA-256 of the file at `path`, read in HASH_CHUNK pieces off this thread. */
async function sha256Of(path: string, canceled: () => boolean): Promise<string> {
  const md: cryptoFramework.Md = cryptoFramework.createMd('SHA256');
  const file: fs.File = fs.openSync(path, fs.OpenMode.READ_ONLY);
  try {
    const buf: ArrayBuffer = new ArrayBuffer(HASH_CHUNK);
    let offset: number = 0;
    for (;;) {
      if (canceled()) {
        return '';
      }
      const n: number = await fs.read(file.fd, buf, { offset: offset, length: HASH_CHUNK });
      if (n <= 0) {
        break;
      }
      // A view, not a copy: the next read waits for this update to finish.
      await md.update({ data: new Uint8Array(buf, 0, n) });
      offset += n;
    }
  } finally {
    fs.closeSync(file);
  }
  const digest: cryptoFramework.DataBlob = await md.digest();
  let hex: string = '';
  for (let i = 0; i < digest.data.length; i++) {
    const b: number = digest.data[i];
    hex += (b < 16 ? '0' : '') + b.toString(16);
  }
  return hex;
}
