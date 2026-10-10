/**
 * The main process: one window, the addon, and the storage behind them.
 *
 * What EntryAbility.ets is on HarmonyOS, with one extra job that platform does
 * not hand back -- a desktop app is asked to quit rather than killed, so the
 * turns still running have to be let go of here.
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
import { describeCore } from './core'
import { cancelAllTurns, registerHandlers, watchWindow } from './ipc'
import * as reader from './reader'
import * as models from './models'
import * as prefs from './prefs'
import * as sessions from './sessions'

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

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit()
  }
})
