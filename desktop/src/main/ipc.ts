/**
 * Every handler behind src/shared/api.ts, and nothing else.
 *
 * One file so the app's whole attack surface reads in one sitting. The renderer
 * is a web page: anything it can ask for, a bug in a dependency can ask for
 * too, so each handler here validates what it was given rather than trusting
 * that the only caller is our own code.
 *
 * Turns are the one stateful thing. bodyclock_chat streams through a callback
 * on a worker thread inside the addon; this file forwards each event to the
 * window that asked for it and drops the turn when that window goes away.
 */
import { BrowserWindow, dialog, ipcMain, shell, WebContents } from 'electron'
import { basename } from 'node:path'
import { CHANNELS, PickedModel } from '../shared/api'
import { ChatMessage, WireMessage } from '../shared/chat'
import { KIND_LOCAL, ModelConfig } from '../shared/provider'
import { CoreStatus, InterfacePrefs } from '../shared/store'
import { core, describeCore } from './core'
import * as keystore from './keystore'
import * as models from './models'
import * as prefs from './prefs'
import * as sessions from './sessions'

/** Live turns, so a window closing can stop the ones it started. */
const turnOwners = new Map<number, WebContents>()

/**
 * Ask every in-flight turn to stop, and say how many there were.
 *
 * Called on quit. Each turn holds a threadsafe function that keeps the main
 * process's event loop alive, so without this, quitting during a long reply
 * waits for the provider to finish answering a window that is already gone.
 */
export function cancelAllTurns(): number {
  try {
    return core().nativeChatCancelAll()
  } catch {
    return 0   // the addon never loaded; there is nothing in flight
  }
}

function handle<A extends unknown[], R>(
  channel: string,
  fn: (...args: A) => R | Promise<R>
): void {
  ipcMain.handle(channel, (_event, ...args) => fn(...(args as A)))
}

/** A model config from the renderer, with only the fields we persist kept. */
function cleanConfig(raw: ModelConfig): ModelConfig {
  if (typeof raw?.id !== 'string' || raw.id.length === 0) {
    throw new Error('a model needs an id')
  }
  const cfg: ModelConfig = {
    id: raw.id,
    vendorId: String(raw.vendorId ?? ''),
    baseUrl: String(raw.baseUrl ?? ''),
    model: String(raw.model ?? '')
  }
  if (raw.kind === KIND_LOCAL) {
    cfg.kind = KIND_LOCAL
    cfg.modelPath = String(raw.modelPath ?? '')
  }
  return cfg
}

export function registerHandlers(): void {
  //----------------------------------------------------------------------------
  // The core
  //----------------------------------------------------------------------------

  handle<[], CoreStatus>(CHANNELS.coreStatus, () => {
    const described = describeCore()
    return {
      ok: described.ok,
      detail: described.ok ? described.version : described.error,
      // Asked only when the addon is there: bodyclock_local_available answers
      // truthfully, but it cannot answer at all through a module that did not
      // load, and "no on-device lane" and "no addon" are different problems.
      localAvailable: described.ok ? core().nativeLocalAvailable() : false,
      keyBackend: keystore.backend(),
      keysAvailable: keystore.available()
    }
  })

  //----------------------------------------------------------------------------
  // Models
  //----------------------------------------------------------------------------

  handle(CHANNELS.modelsList, () => models.list())
  handle<[ModelConfig], void>(CHANNELS.modelsAdd, (cfg) => models.add(cleanConfig(cfg)))
  handle<[string], void>(CHANNELS.modelsRemove, (id) => models.remove(String(id)))
  handle<[string], void>(CHANNELS.modelsRemoveVendor, (vendorId) => {
    const id = String(vendorId)
    models.removeVendor(id)
    keystore.deleteKey(id)
  })
  handle(CHANNELS.modelsActiveId, () => models.activeId())
  handle<[string], void>(CHANNELS.modelsSetActiveId, (id) => models.setActiveId(String(id)))
  handle<[string[]], string[]>(CHANNELS.modelsVendorsWithKeys, (ids) =>
    models.vendorsWithKeys(Array.isArray(ids) ? ids.map(String) : [])
  )

  handle<[], PickedModel | null>(CHANNELS.modelsPickLocal, async () => {
    const win = BrowserWindow.getFocusedWindow()
    const opts = {
      title: 'Pick a GGUF model',
      // The file stays where it is: it is referenced, never copied, and
      // removing the model never deletes it -- the same promise the HarmonyOS
      // app makes, and on a desktop it matters more, because these are the
      // user's own multi-gigabyte downloads.
      properties: ['openFile' as const],
      filters: [
        { name: 'GGUF models', extensions: ['gguf'] },
        { name: 'All files', extensions: ['*'] }
      ]
    }
    const result =
      win === null
        ? await dialog.showOpenDialog(opts)
        : await dialog.showOpenDialog(win, opts)

    if (result.canceled || result.filePaths.length === 0) {
      return null
    }
    const path = result.filePaths[0]
    return { path, name: basename(path) }
  })

  handle<[string], string>(
    CHANNELS.modelsPreload,
    (id) =>
      new Promise<string>((resolve) => {
        // The addon runs this on a worker thread and calls back exactly once.
        // Resolved with a string rather than rejected on failure: a model that
        // will not load is something to show beside the picker, not an
        // exception the renderer has to catch around a preload it fired and
        // forgot.
        core().nativeLocalLoad(String(id), (type, content) => {
          resolve(type === 'end' ? '' : content.length > 0 ? content : 'the model would not load')
        })
      })
  )

  //----------------------------------------------------------------------------
  // Keys
  //----------------------------------------------------------------------------

  handle(CHANNELS.keysSave, (vendorId: string, key: string) =>
    keystore.saveKey(String(vendorId), String(key))
  )
  handle(CHANNELS.keysHas, (vendorId: string) => keystore.hasKey(String(vendorId)))
  handle(CHANNELS.keysRemove, (vendorId: string) => keystore.deleteKey(String(vendorId)))

  //----------------------------------------------------------------------------
  // Sessions
  //----------------------------------------------------------------------------

  handle(CHANNELS.sessionsIndex, () => sessions.index())
  handle(CHANNELS.sessionsMessages, (id: string) => sessions.messages(String(id)))
  handle(CHANNELS.sessionsSave, (id: string, msgs: ChatMessage[]) =>
    sessions.save(String(id), Array.isArray(msgs) ? msgs : [])
  )
  handle(CHANNELS.sessionsRemove, (id: string) => sessions.remove(String(id)))
  handle(CHANNELS.sessionsClear, () => sessions.clear())

  //----------------------------------------------------------------------------
  // Preferences
  //----------------------------------------------------------------------------

  handle(CHANNELS.prefsGet, () => prefs.get())
  handle(CHANNELS.prefsSet, (patch: Partial<InterfacePrefs>) => prefs.set(patch ?? {}))

  //----------------------------------------------------------------------------
  // Chat
  //----------------------------------------------------------------------------

  ipcMain.handle(
    CHANNELS.chatStart,
    (event, providerId: string, messages: WireMessage[], systemPrompt: string) => {
      const sender = event.sender
      const wire = (Array.isArray(messages) ? messages : []).map((m) => ({
        role: String(m.role),
        content: String(m.content)
      }))

      // Assigned below and read inside the callback. Safe in that order: the
      // addon queues every event onto this thread, which is inside nativeChat
      // until it returns, so no event can be delivered before the id exists.
      let turnId = 0

      // No grammar: a conversation is free text by definition, and the header
      // says a remote provider would ignore one anyway.
      turnId = core().nativeChat(
        String(providerId),
        JSON.stringify(wire),
        String(systemPrompt ?? ''),
        '',
        (type, content) => {
          if (sender.isDestroyed()) {
            // The window went while the turn was running. Stop the turn rather
            // than stream into nothing -- the core tears the transfer down at
            // the next chunk.
            core().nativeChatCancel(turnId)
            return
          }
          sender.send(CHANNELS.chatEvent, { turnId, type, content })
          if (type === 'end' || type === 'aborted') {
            turnOwners.delete(turnId)
          }
        }
      )

      turnOwners.set(turnId, sender)
      return turnId
    }
  )

  handle<[number], void>(CHANNELS.chatCancel, (turnId) => {
    core().nativeChatCancel(Number(turnId))
  })

  //----------------------------------------------------------------------------
  // Shell
  //----------------------------------------------------------------------------

  handle<[string], void>(CHANNELS.openExternal, async (url) => {
    // Only the two schemes a vendor's sign-up page uses. Without this check the
    // renderer could ask the OS to open file: or a custom scheme, which is the
    // usual way an Electron app is talked into running something.
    const parsed = new URL(String(url))
    if (parsed.protocol !== 'https:' && parsed.protocol !== 'http:') {
      throw new Error(`refusing to open a ${parsed.protocol} URL`)
    }
    await shell.openExternal(parsed.toString())
  })
}

/**
 * Stop the turns a window owns when it closes.
 *
 * The handler above notices a destroyed sender at the NEXT event, which for a
 * model that has gone quiet could be a long wait. Hooking the window is what
 * makes closing it immediate.
 */
export function watchWindow(win: BrowserWindow): void {
  win.webContents.on('destroyed', () => {
    for (const [turnId, owner] of turnOwners) {
      if (owner === win.webContents) {
        core().nativeChatCancel(turnId)
        turnOwners.delete(turnId)
      }
    }
  })
}
