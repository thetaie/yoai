/**
 * Every handler behind src/shared/api.ts, and nothing else.
 *
 * One file so the app's whole attack surface reads in one sitting. The renderer
 * is a web page: anything it can ask for, a bug in a dependency can ask for
 * too, so each handler here validates what it was given rather than trusting
 * that the only caller is our own code.
 *
 * Turns are the one stateful thing. yoai_chat streams through a callback
 * on a worker thread inside the addon; this file forwards each event to the
 * window that asked for it and drops the turn when that window goes away.
 */
import { app, BrowserWindow, dialog, ipcMain, shell, WebContents } from 'electron'
import { basename, dirname } from 'node:path'
import {
  CHANNELS,
  ChatSettings,
  ChatSettingsPatch,
  LocalOffer,
  ModelDownload,
  OmniStatus,
  PickedModel,
  ReaderStatus,
  TtsSlot,
  TtsStatus
} from '../shared/api'
import { ChatMessage, WireMessage } from '../shared/chat'
import { KIND_LOCAL, LOCAL_VENDOR_ID, ModelConfig, modelId } from '../shared/provider'
import { CoreStatus, InterfacePrefs } from '../shared/store'
import { core, describeCore } from './core'
import * as chat from './chat'
import * as keystore from './keystore'
import * as llmfiles from './llmfiles'
import * as models from './models'
import * as omni from './omni'
import * as reader from './reader'
import * as prefs from './prefs'
import * as sessions from './sessions'
import * as tts from './tts'
import * as ttsfiles from './ttsfiles'

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
    return 0 // the addon never loaded; there is nothing in flight
  }
}

function handle<A extends unknown[], R>(channel: string, fn: (...args: A) => R | Promise<R>): void {
  ipcMain.handle(channel, (_event, ...args) => fn(...(args as A)))
}

/**
 * Load the voice again from the stored paths, letting go of what was loaded
 * first: it was a file just replaced, or the other mode's talker. Unawaited,
 * and a no-op while a slot the mode needs is still empty.
 */
function reloadVoice(): void {
  reader.unload()
  void reader.autoLoadVoice().then((reason) => {
    if (reason.length > 0) {
      console.error(`[yoai] the voice would not load: ${reason}`)
    }
    models.warmActive()   // whether replies are read may have changed
  })
}

/**
 * Store `path` in `slot`, fill the other empty slots from its folder, and load.
 *
 * The folder is looked in because the files usually sit together, and the
 * second pick is a step a new user can only get wrong. A file already stored
 * is never replaced this way, only an empty slot filled.
 */
function useModelFile(slot: TtsSlot, path: string): void {
  tts.setPath(slot, path)
  const beside = ttsfiles.findIn(dirname(path), tts.emptySlots())
  for (const [other, p] of Object.entries(beside) as Array<[TtsSlot, string]>) {
    tts.setPath(other, p)
  }
  // Picking the second of the two is the whole configuration, so it is also
  // the moment to load -- pressing 载入 afterwards was a step that could only
  // be forgotten.
  reloadVoice()
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
      // Asked only when the addon is there: yoai_local_available answers
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
  handle<[string], void>(CHANNELS.modelsSetActiveId, (id) => {
    models.setActiveId(String(id))
    // Switching to an on-device model starts reading its weights now rather
    // than on the first message. Unawaited: the picker has already closed.
    models.autoPreload()
  })
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
      win === null ? await dialog.showOpenDialog(opts) : await dialog.showOpenDialog(win, opts)

    if (result.canceled || result.filePaths.length === 0) {
      return null
    }
    const path = result.filePaths[0]
    return { path, name: basename(path) }
  })

  handle(CHANNELS.modelsLoadStates, () => models.loadStates())

  handle<[], LocalOffer>(CHANNELS.modelsOffer, () => llmfiles.offer())

  handle<[], boolean>(CHANNELS.modelsDownload, async () => {
    // The file, and so the URL, is decided here and not sent by the page, as
    // for the speech models.
    if (!core().nativeLocalAvailable() || llmfiles.downloadStatus().running) {
      return false
    }
    const win = BrowserWindow.getFocusedWindow()
    const opts = {
      title: 'Pick a folder to download the model into',
      // The speech models' folder: the files usually sit together.
      defaultPath: tts.downloadDir() || app.getPath('downloads'),
      properties: ['openDirectory' as const, 'createDirectory' as const]
    }
    const result =
      win === null ? await dialog.showOpenDialog(opts) : await dialog.showOpenDialog(win, opts)
    if (result.canceled || result.filePaths.length === 0) {
      return false
    }
    const dir = result.filePaths[0]
    tts.setDownloadDir(dir)
    void llmfiles.download(dir, (path) => {
      const name = basename(path)
      const id = modelId(LOCAL_VENDOR_ID, name)
      models.add({
        id,
        vendorId: LOCAL_VENDOR_ID,
        baseUrl: '',
        model: name,
        kind: KIND_LOCAL,
        modelPath: path
      })
      // Selected only when nothing else was (models.add), and then read now,
      // as switching to it would.
      if (models.activeId() === id) {
        models.autoPreload()
      }
    })
    return true
  })

  handle<[], ModelDownload>(CHANNELS.modelsDownloadStatus, () => llmfiles.downloadStatus())
  handle<[], void>(CHANNELS.modelsCancelDownload, () => llmfiles.cancelDownload())

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

  handle<[], ChatSettings>(CHANNELS.chatSettingsGet, () => chat.get())
  handle<[ChatSettingsPatch], ChatSettings>(CHANNELS.chatSettingsSet, (patch) => {
    // Validated: null is a reset and a string is a prompt; anything else from
    // the page leaves the stored prompt alone.
    const p = patch?.systemPrompt
    const next = chat.set(p === null || typeof p === 'string' ? { systemPrompt: p } : {})
    models.warmActive()   // the front the next turn shares has just changed
    return next
  })

  ipcMain.handle(
    CHANNELS.chatStart,
    (event, providerId: string, messages: WireMessage[], reading: boolean) => {
      const sender = event.sender
      const wire = (Array.isArray(messages) ? messages : []).map((m) => ({
        role: String(m.role),
        content: String(m.content)
      }))

      // What the model is actually given, at the one point where the page's
      // intent becomes the core's input. The renderer replays the whole
      // transcript every turn and the core keeps no session state, so a reply
      // that remembers a conversation the screen has cleared is visible here
      // and nowhere else: the roles say how many turns came along.
      console.log(
        `[chat] ${wire.length} message(s): ${wire.map((m) => `${m.role}/${m.content.length}`).join(' ')}`
      )

      // Assigned below and read inside the callback. Safe in that order: the
      // addon queues every event onto this thread, which is inside nativeChat
      // until it returns, so no event can be delivered before the id exists.
      let turnId = 0

      // No grammar: a conversation is free text by definition, and the header
      // says a remote provider would ignore one anyway.
      turnId = core().nativeChat(
        String(providerId),
        JSON.stringify(wire),
        chat.turnPrompt(reading === true),
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

  handle<[], number>(CHANNELS.chatForget, () => models.forgetAll())

  //----------------------------------------------------------------------------
  // Speech output
  //----------------------------------------------------------------------------

  handle<[], TtsStatus>(CHANNELS.ttsStatus, () => tts.status())

  handle<[TtsSlot], PickedModel | null>(CHANNELS.ttsPickModel, async (which) => {
    // Validated rather than trusted: this value chooses which stored path gets
    // overwritten, and the renderer is a web page.
    //
    // REJECTED, not defaulted. An earlier version read `which === 'tokenizer' ?
    // 'tokenizer' : 'talker'`, which turns an unrecognised value into a valid
    // one -- so a renderer still sending the old name wrote the tokenizer into
    // the TALKER slot, and the engine failed two steps later with "talker GGUF
    // has wrong architecture". Falling back to a member of the enum makes a
    // protocol mismatch indistinguishable from a choice.
    if (which !== 'talker' && which !== 'customTalker' && which !== 'tokenizer') {
      throw new Error(`unknown model slot '${String(which)}'`)
    }
    const slot = which
    const win = BrowserWindow.getFocusedWindow()
    const opts = {
      title:
        slot === 'talker'
          ? 'Pick the Base talker model'
          : slot === 'customTalker'
            ? 'Pick the CustomVoice talker model'
            : 'Pick the tokenizer model',
      properties: ['openFile' as const],
      filters: [
        { name: 'GGUF models', extensions: ['gguf'] },
        { name: 'All files', extensions: ['*'] }
      ]
    }
    const result =
      win === null ? await dialog.showOpenDialog(opts) : await dialog.showOpenDialog(win, opts)

    if (result.canceled || result.filePaths.length === 0) {
      return null
    }
    const path = result.filePaths[0]
    useModelFile(slot, path)
    return { path, name: basename(path) }
  })

  handle<[], PickedModel | null>(CHANNELS.ttsPickAny, async () => {
    const win = BrowserWindow.getFocusedWindow()
    const opts = {
      title: 'Pick one of the speech model files',
      properties: ['openFile' as const],
      filters: [
        { name: 'GGUF models', extensions: ['gguf'] },
        { name: 'All files', extensions: ['*'] }
      ]
    }
    const result =
      win === null ? await dialog.showOpenDialog(opts) : await dialog.showOpenDialog(win, opts)
    if (result.canceled || result.filePaths.length === 0) {
      return null
    }
    const path = result.filePaths[0]
    // The slot its name says, else the first one missing: the one the screen
    // listed first. A user with the files picks whichever comes first in the
    // folder, and the tokenizer stored as the talker fails only at load.
    useModelFile(ttsfiles.slotOf(path) ?? tts.missingSlots()[0] ?? tts.talkerSlot(), path)
    return { path, name: basename(path) }
  })

  handle<[], boolean>(CHANNELS.ttsDownload, async () => {
    // Decided here, not sent by the page: which files, and so which URLs, are
    // fetched is not a renderer's choice.
    const slots = tts.missingSlots()
    if (slots.length === 0 || ttsfiles.downloadStatus().running) {
      return false
    }
    const win = BrowserWindow.getFocusedWindow()
    const opts = {
      title: 'Pick a folder to download the speech models into',
      defaultPath: tts.downloadDir() || app.getPath('downloads'),
      properties: ['openDirectory' as const, 'createDirectory' as const]
    }
    const result =
      win === null ? await dialog.showOpenDialog(opts) : await dialog.showOpenDialog(win, opts)
    if (result.canceled || result.filePaths.length === 0) {
      return false
    }
    const dir = result.filePaths[0]
    tts.setDownloadDir(dir)
    void ttsfiles.download(dir, slots, (paths) => {
      for (const [slot, p] of Object.entries(paths) as Array<[TtsSlot, string]>) {
        tts.setPath(slot, p)
      }
      reloadVoice()
    })
    return true
  })

  handle<[], ModelDownload>(CHANNELS.ttsDownloadStatus, () => ttsfiles.downloadStatus())
  handle<[], void>(CHANNELS.ttsCancelDownload, () => ttsfiles.cancelDownload())

  handle<[], string>(CHANNELS.ttsLoad, () => tts.load())
  handle<[], void>(CHANNELS.ttsUnload, () => tts.unload())
  handle<[], void>(CHANNELS.ttsCancel, () => tts.cancel())
  handle<[string], void>(CHANNELS.ttsSetVoice, (text) => tts.setVoice(String(text ?? '')))
  handle<[boolean], void>(CHANNELS.ttsSetAutoSpeak, (on) => tts.setAutoSpeak(on === true))
  handle<[string], void>(CHANNELS.ttsSetSpeaker, (name) => {
    tts.setSpeaker(String(name ?? ''))
    reader.applyVoice()   // the reader is what speaks now; from its next reply
  })
  handle<[string], void>(CHANNELS.ttsSetVoiceMode, (mode) => {
    // Validated like the model slot: it picks which talker gets loaded.
    if (mode !== 'base' && mode !== 'custom') {
      throw new Error(`unknown voice mode '${String(mode)}'`)
    }
    tts.setVoiceMode(mode)
    // The other talker: what is loaded is the wrong one now.
    reloadVoice()
  })
  handle<[number], void>(CHANNELS.ttsSetSeed, (seed) => {
    tts.setSeed(Number(seed))
    reader.applySeed()
  })

  // Audio goes to the window that asked, as it arrives. Nothing is buffered
  // here: the renderer owns the AudioContext, and holding samples in the main
  // process would only add a copy and a reason for them to be late.
  ipcMain.handle(CHANNELS.ttsSay, (event, text: string) => {
    const sender = event.sender
    // Timed on this side as well as in the renderer. The engine reports when it
    // handed over the first chunk; these say when THIS process forwarded it and
    // how the rest were spaced. Three timestamps for one hop each, which is the
    // only way to tell a slow engine from a stalled delivery.
    const askedAt = Date.now()
    let forwarded = 0

    // What the engine is ACTUALLY given, which is not the reply on screen: the
    // renderer strips markdown and emoji first. Logged because the difference
    // is invisible otherwise -- a reply that reads its own asterisks out loud
    // looks like a synthesis fault until you can see the string that caused it.
    const spoken = String(text ?? '')
    console.log(
      `[tts] say ${spoken.length} chars: ${JSON.stringify(
        spoken.length > 160 ? `${spoken.slice(0, 160)}…` : spoken
      )}`
    )

    return tts.say(spoken, (type, payload) => {
      if (sender.isDestroyed()) {
        // The window went mid-utterance. Stop generating rather than speak into
        // nothing -- the core drops it at the next chunk.
        tts.cancel()
        return
      }
      if (type === 'audio') {
        forwarded++
        if (forwarded === 1) {
          console.log(`[tts] chunk 1 forwarded ${Date.now() - askedAt} ms after say`)
        }
        sender.send(CHANNELS.ttsEvent, { type, pcm: payload as Float32Array })
      } else if (type === 'end') {
        console.log(`[tts] ${forwarded} chunk(s) forwarded, last at ${Date.now() - askedAt} ms`)
        sender.send(CHANNELS.ttsEvent, { type, stats: String(payload) })
      } else {
        sender.send(CHANNELS.ttsEvent, { type: 'error', error: String(payload) })
      }
    })
  })

  //----------------------------------------------------------------------------
  // Omni
  //----------------------------------------------------------------------------

  handle<[], OmniStatus>(CHANNELS.omniStatus, () => omni.status())
  handle<[], string>(CHANNELS.omniLoad, () => omni.load())
  handle<[], void>(CHANNELS.omniUnload, () => omni.unload())

  handle<[{ ditSteps?: number; enabled?: boolean }], OmniStatus>(CHANNELS.omniSet, (patch) => {
    if (typeof patch?.ditSteps === 'number') omni.setDitSteps(patch.ditSteps)
    if (typeof patch?.enabled === 'boolean') omni.setEnabled(patch.enabled)
    return omni.status()
  })

  handle<[], string | null>(CHANNELS.omniPickDir, async () => {
    const win = BrowserWindow.getFocusedWindow()
    // A DIRECTORY, unlike every other picker here: an MNN model is a folder of
    // graphs, not one file.
    const opts = { title: 'Pick the omni model directory', properties: ['openDirectory' as const] }
    const result =
      win === null ? await dialog.showOpenDialog(opts) : await dialog.showOpenDialog(win, opts)
    if (result.canceled || result.filePaths.length === 0) {
      return null
    }
    omni.setModelDir(result.filePaths[0])
    return result.filePaths[0]
  })

  // Text in, speech out, with a silence where the recording will go.
  //
  // ask() requires audio -- the model is multimodal and the prompt alone is not
  // a valid input -- so until there is a microphone the question travels as the
  // prompt and the audio is half a second of nothing. Everything except
  // recognition is exercised by that, which is what this lane is being asked
  // about.
  ipcMain.handle(CHANNELS.omniAsk, (event, prompt: string) => {
    const sender = event.sender
    const silence = new Int16Array(8000) // 0.5 s at 16 kHz
    return omni.ask(silence, String(prompt ?? ''), true, (type, payload) => {
      if (sender.isDestroyed()) {
        return
      }
      if (type === 'audio') {
        sender.send(CHANNELS.omniEvent, { type, pcm: payload as Float32Array })
      } else if (type === 'text') {
        sender.send(CHANNELS.omniEvent, { type, text: String(payload) })
      } else if (type === 'end') {
        sender.send(CHANNELS.omniEvent, { type, stats: String(payload) })
      } else {
        sender.send(CHANNELS.omniEvent, { type: 'error', error: String(payload) })
      }
    })
  })

  handle<[], void>(CHANNELS.omniCancel, () => omni.cancel())

  //----------------------------------------------------------------------------
  // Reading a reply while it is still being written
  //----------------------------------------------------------------------------

  handle<[], ReaderStatus>(CHANNELS.readerStatus, () => reader.status())
  handle<[], string>(CHANNELS.readerLoad, () => reader.load())
  handle<[], void>(CHANNELS.readerUnload, () => reader.unload())

  handle<[{ enabled?: boolean; minChars?: number; temperature?: number; style?: string }], ReaderStatus>(
    CHANNELS.readerSet,
    (patch) => {
      if (typeof patch?.enabled === 'boolean') {
        reader.setEnabled(patch.enabled)
        models.warmActive()   // reading adds the spoken-style paragraph, or drops it
      }
      if (typeof patch?.minChars === 'number') reader.setMinChars(patch.minChars)
      if (typeof patch?.temperature === 'number') reader.setTemperature(patch.temperature)
      if (typeof patch?.style === 'string') reader.setStyle(patch.style)
      return reader.status()
    }
  )

  handle<[], string | null>(CHANNELS.readerPickReference, async () => {
    const win = BrowserWindow.getFocusedWindow()
    const opts = {
      title: 'Pick a reference voice',
      properties: ['openFile' as const],
      // A recording, mono 16-bit at 24 kHz, or an embedding already extracted
      // from one -- qwentts.cpp's qwen-codec writes the same .spk.
      filters: [{ name: 'Recording or embedding', extensions: ['wav', 'spk'] }]
    }
    const result =
      win === null ? await dialog.showOpenDialog(opts) : await dialog.showOpenDialog(win, opts)
    if (result.canceled || result.filePaths.length === 0) {
      return null
    }
    return reader.setReference(result.filePaths[0])
  })
  handle<[string], string>(CHANNELS.readerUseBundled, (name) =>
    typeof name === 'string' ? reader.useBundled(name) : Promise.resolve('not a voice name')
  )
  handle<[Uint8Array], string>(CHANNELS.readerUseRecording, (wav) =>
    wav instanceof Uint8Array ? reader.useRecording(wav) : Promise.resolve('not a recording')
  )

  // The stream itself. begin() installs a sink that forwards to the window that
  // asked; feed() and finish() are fire-and-forget from the renderer's side,
  // and the audio comes back on readerEvent either way.
  ipcMain.handle(CHANNELS.readerBegin, (event) => {
    const sender = event.sender
    const askedAt = Date.now()
    let forwarded = 0

    return reader.begin(
      (pcm, sentence) => {
        if (sender.isDestroyed()) {
          return
        }
        forwarded++
        if (forwarded === 1) {
          console.log(`[reader] first audio forwarded at ${Date.now() - askedAt} ms`)
        }
        sender.send(CHANNELS.readerEvent, { type: 'audio', pcm, sentence })
      },
      (stats, error) => {
        if (sender.isDestroyed()) {
          return
        }
        if (error.length > 0) {
          sender.send(CHANNELS.readerEvent, { type: 'error', error })
          return
        }
        console.log(`[reader] ${stats}`)
        sender.send(CHANNELS.readerEvent, { type: 'end', stats })
      }
    )
  })

  handle<[string], boolean>(CHANNELS.readerFeed, (delta) => reader.feed(String(delta ?? '')))
  handle<[], void>(CHANNELS.readerFinish, () => reader.finish())
  handle<[], void>(CHANNELS.readerCancel, () => reader.cancel())

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
