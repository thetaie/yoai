/**
 * The main process: one window, the addon, and the storage behind them.
 *
 * What EntryAbility.ets is on HarmonyOS, with one extra job that platform does
 * not hand back -- a desktop app is asked to quit rather than killed, so the
 * turns still running have to be let go of here, and the engines freed before
 * the process exits.
 *
 * Everything privileged lives on this side. The window is created with
 * contextIsolation on, node integration off and the sandbox in force, which is
 * not ceremony: the core's registry holds the user's API keys in the memory of
 * THIS process, and the page's only way to reach anything is the handful of
 * named calls in ipc.ts.
 */
import { app, BrowserWindow, nativeTheme, session, shell } from 'electron'
import { existsSync, mkdirSync, readdirSync, renameSync, rmdirSync } from 'node:fs'
import { join } from 'node:path'
import palettes from '../../../../design/palettes.json'
import * as asr from './asr'
import { core, describeCore } from './core'
import * as duplex from './duplex'
import { cancelAllTurns, registerHandlers, watchWindow } from './ipc'
import * as omni from './omni'
import * as reader from './reader'
import * as models from './models'
import * as prefs from './prefs'
import * as sessions from './sessions'
import * as tts from './tts'

/**
 * The data directory of the name this app had before, carried over once.
 *
 * Electron names the directory after package.json's "name", which was
 * bodyclock-desktop before it became yoai-desktop. Without this, an existing
 * install's keys, sessions and settings would still be on disk and simply not
 * be found. Entry by entry rather than one rename, because Electron may have
 * created the new directory already; nothing that exists there is replaced.
 *
 * Before anything reads userData -- nothing does while the modules above load.
 */
function adoptOldUserData(): void {
  const current = app.getPath('userData')
  const old = join(app.getPath('appData'), 'bodyclock-desktop')
  if (!existsSync(old)) return
  try {
    mkdirSync(current, { recursive: true })
    for (const entry of readdirSync(old)) {
      if (!existsSync(join(current, entry))) {
        renameSync(join(old, entry), join(current, entry))
      }
    }
    if (readdirSync(old).length === 0) rmdirSync(old)
    console.log(`[yoai] carried ${old} over to ${current}`)
  } catch (e) {
    console.error(`[yoai] could not carry ${old} over to ${current}: ${e}`)
  }
}
adoptOldUserData()

/**
 * The window's own background, before a single stylesheet has loaded.
 *
 * Read from design/palettes.json for the same reason the renderer's colors are
 * -- it is the source of record, and a hex typed here would be the one color in
 * the app that nothing regenerates. Without it the frame paints white, which on
 * a dark palette is a flash at every launch.
 */
function windowBackground(): string {
  const p = prefs.get()
  const dark =
    p.colorMode === 'dark' || (p.colorMode === 'system' && nativeTheme.shouldUseDarkColors)
  const want = dark ? 'dark' : 'light'

  // A palette key contains its family name -- daylight-arctic, midnight-arctic,
  // quiet-day -- which is the same rule design/scripts/make-css-palettes.py
  // groups by.
  const themes = Object.entries(palettes.themes)
  const pick =
    themes.find(([key, t]) => key.includes(p.paletteFamily) && t.mode === want) ??
    themes.find(([, t]) => t.mode === want) ??
    themes[0]
  return pick[1]['surface-page']
}

/**
 * Send a content security policy with every document this app loads.
 *
 * `default-src 'none'` is the line that matters, and it is worth stating what
 * it buys: the page cannot fetch anything, from anywhere. That is what makes
 * "there is no YoAI server" checkable instead of promised -- a dependency
 * that decided to phone home would be stopped by the browser engine rather than
 * by anyone reading the dependency. The model calls are unaffected; they are
 * made by libcurl inside the core, in the main process, where no CSP applies.
 *
 * Development needs two holes and gets them only there: Vite serves its refresh
 * runtime as an inline script, and HMR needs a socket back to the dev server.
 */
function applyContentSecurityPolicy(devUrl: string | undefined): void {
  const dev = devUrl !== undefined && devUrl.length > 0

  const policy = [
    "default-src 'none'",
    dev ? `script-src 'self' 'unsafe-inline' ${devUrl}` : "script-src 'self'",
    // Vite injects the stylesheet inline in development, and React sets a few
    // style attributes in both; neither is a script vector.
    "style-src 'self' 'unsafe-inline'",
    "img-src 'self' data:",
    "font-src 'self'",
    dev ? `connect-src 'self' ${devUrl} ws://localhost:* ws://127.0.0.1:*` : "connect-src 'none'",
    "form-action 'none'",
    "frame-ancestors 'none'"
  ].join('; ')

  session.defaultSession.webRequest.onHeadersReceived((details, callback) => {
    callback({
      responseHeaders: {
        ...details.responseHeaders,
        'Content-Security-Policy': [policy]
      }
    })
  })
}

function createWindow(): BrowserWindow {
  const win = new BrowserWindow({
    width: 1120,
    height: 780,
    minWidth: 720,
    minHeight: 520,
    // Shown once the renderer has something to draw, so the launch is one step
    // rather than a blank frame and then the app.
    show: false,
    backgroundColor: windowBackground(),
    autoHideMenuBar: true,
    webPreferences: {
      preload: join(__dirname, '../preload/index.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true
    }
  })

  win.once('ready-to-show', () => win.show())

  // A chat client renders text from a model and links from a vendor page.
  // Neither is allowed to move this window or open another one: anything the
  // user clicks goes to their browser, where it is ordinary web content.
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (url.startsWith('https://') || url.startsWith('http://')) {
      void shell.openExternal(url)
    }
    return { action: 'deny' }
  })
  win.webContents.on('will-navigate', (event, url) => {
    if (url !== win.webContents.getURL()) {
      event.preventDefault()
    }
  })

  watchWindow(win)

  const devUrl = process.env['ELECTRON_RENDERER_URL']
  if (devUrl !== undefined && devUrl.length > 0) {
    void win.loadURL(devUrl)
  } else {
    void win.loadFile(join(__dirname, '../renderer/index.html'))
  }
  return win
}

app.whenReady().then(() => {
  // Named so the OS shows the app rather than "Electron" in notifications and
  // the Windows taskbar's grouping.
  app.setAppUserModelId('ai.yoai.desktop')

  const described = describeCore()
  if (described.ok) {
    console.log(`[yoai] ${described.version}`)
    // The registry is process memory and empty after every start; this is what
    // refills it, and it has to happen before the first turn can name a model.
    models.syncToCore()
  } else {
    // Not fatal, and not a dialog. The window opens, asks for coreStatus and
    // shows the reason in place of the composer -- which is where someone
    // would look for it, and is one screenshot instead of two.
    console.error(`[yoai] the addon did not load: ${described.error}`)
  }

  sessions.sweepOrphans()
  applyContentSecurityPolicy(process.env['ELECTRON_RENDERER_URL'])
  registerHandlers()
  createWindow()

  // Configured IS enabled. Having picked a model, or a pair of voice models,
  // and then having to press a button every run is a step that exists only to
  // be forgotten -- and the thing it delays is the first reply, which is the
  // one moment somebody is already waiting.
  //
  // After createWindow so the window is up while the weights are read, and
  // unawaited because nothing here depends on the outcome.
  if (described.ok) {
    models.autoPreload()
    // Recognition too, for the microphone button: a quarter of a second for
    // SenseVoice at q8, on its own worker.
    void asr.autoLoad().then((reason) => {
      if (reason.length > 0) {
        console.error(`[yoai] recognition would not load: ${reason}`)
      }
    })
    // Into whichever lane should speak; see reader.autoLoadVoice().
    void reader.autoLoadVoice().then((reason) => {
      if (reason.length > 0) {
        console.error(`[yoai] the voice would not load: ${reason}`)
      }
      // A voice that is up means replies are read, which changes the prompt
      // they are sent with; whichever of the two loads finishes last warms it.
      models.warmActive()
    })
  }

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      createWindow()
    }
  })
})

/**
 * Let go of the turns still running before the process tears down.
 *
 * Each one holds a threadsafe function inside the addon, which keeps this
 * process's event loop alive: quitting mid-reply without this waits for the
 * provider to finish answering a window that is already gone. Cancelling is a
 * request -- the turn ends at its next event -- so this does not block, it only
 * stops the wait from being unbounded.
 */
app.on('before-quit', () => {
  const stopped = cancelAllTurns()
  if (stopped > 0) {
    console.log(`[yoai] asked ${stopped} in-flight turn(s) to stop`)
  }
})

/**
 * How long quitting waits for native work to end before it stops trying to
 * free the engines. What was cancelled ends within a token or a block of
 * speech; what cannot be cancelled is a model load or a warm-up, which here
 * took 1.4s and 0.6s.
 */
const QUIT_WAIT_MS = 10000

/** Set once the engines have been dealt with, so the next will-quit goes on. */
let released = false
/** The release under way, so a second quit while it runs does not start another. */
let releasing: Promise<void> | null = null

/**
 * Free the engines while the process is still running, not as it exits.
 *
 * Left loaded, they are freed by static destructors during the exit itself,
 * and every GPU backend objects to that. On Windows the driver's threads are
 * already gone: Intel's Vulkan driver failed fast, and the error report held
 * the closing app for 12s. ggml's Metal backend asserts that each buffer was
 * freed before its device. CUDA's runtime has shut itself down by then, which
 * ggml turns into an abort. The core no longer frees them at exit at all (see
 * chats() in src/yoai.cpp), which covers Windows; Metal still needs them freed
 * here, and so does CUDA on Linux, where a worker still running at exit keeps
 * calling into a runtime that is gone.
 *
 * Only once no worker is running: unloading under a reading or a load frees
 * what it is using. So the work is cancelled where it can be and waited for,
 * up to QUIT_WAIT_MS; past that the engines are left to the core.
 */
app.on('will-quit', (event) => {
  if (released) {
    return
  }
  event.preventDefault()
  if (releasing !== null) {
    return
  }
  releasing = releaseEngines()
    .catch((err: unknown) => {
      console.error('[yoai] freeing the engines failed:', err)
    })
    .finally(() => {
      released = true
      app.quit()
    })
})

async function releaseEngines(): Promise<void> {
  try {
    core()
  } catch {
    return // the addon never loaded; nothing is held
  }
  const started = Date.now()

  // Turns were cancelled by before-quit. A reading is cancelled and, if the
  // window closed before finishing it, finished, since nothing else will. A
  // voice session stops taking audio, so it starts no recognition.
  duplex.stop()
  reader.abandon()
  tts.cancel()
  omni.cancel()

  if (!(await workersDone(started + QUIT_WAIT_MS))) {
    console.warn(
      `[yoai] native work still running after ${QUIT_WAIT_MS} ms; quitting without freeing the engines`
    )
    return
  }
  reader.unload()
  tts.unload()
  omni.unload()
  models.releaseAll()
  console.log(`[yoai] engines released in ${Date.now() - started} ms`)
}

/**
 * True once no worker has been running for two polls in a row, false at the
 * deadline. Two, because a worker's end reaches this thread as a queued event
 * whose handler may start the next one -- a voice load is followed by a
 * warm-up -- and a single poll can fall between them.
 */
async function workersDone(deadline: number): Promise<boolean> {
  let idle = 0
  while (Date.now() < deadline) {
    idle = core().nativeWorkers() === 0 ? idle + 1 : 0
    if (idle >= 2) {
      return true
    }
    await new Promise((resolve) => setTimeout(resolve, 50))
  }
  return false
}

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit()
  }
})
