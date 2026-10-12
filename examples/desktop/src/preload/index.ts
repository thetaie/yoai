/**
 * The bridge, and the only thing the page can see.
 *
 * Runs in the renderer's process with contextIsolation on, so what is handed to
 * `window.yoai` is a plain object of functions and nothing else: no
 * ipcRenderer, no module, no require. Every call forwards to a named handler in
 * src/main/ipc.ts, and the shape is src/shared/api.ts -- so the surface is
 * declared in one file, implemented in another, and exposed here.
 *
 * Nothing in this file decides anything. If a rule belongs somewhere, it
 * belongs in the main process: a check that lives on this side of the bridge is
 * a check that runs inside the process an attacker would already own.
 */
import { contextBridge, ipcRenderer } from 'electron'
import {
  API_KEY,
  YoaiApi,
  CHANNELS,
  ChatSettings,
  LocalOffer,
  ModelDownload,
  ModelLoadState,
  OmniEvent,
  OmniStatus,
  PickedModel,
  ReaderEvent,
  ReaderStatus,
  TtsEvent,
  TtsSlot,
  TtsStatus
} from '../shared/api'
import type { ChatEvent, ChatMessage, WireMessage } from '../shared/chat'
import type { ModelConfig } from '../shared/provider'
import type { CoreStatus, InterfacePrefs, KeySaveResult, SessionMeta } from '../shared/store'

const api: YoaiApi = {
  coreStatus: () => ipcRenderer.invoke(CHANNELS.coreStatus) as Promise<CoreStatus>,

  models: {
    list: () => ipcRenderer.invoke(CHANNELS.modelsList) as Promise<ModelConfig[]>,
    add: (cfg) => ipcRenderer.invoke(CHANNELS.modelsAdd, cfg) as Promise<void>,
    remove: (id) => ipcRenderer.invoke(CHANNELS.modelsRemove, id) as Promise<void>,
    removeVendor: (vendorId) =>
      ipcRenderer.invoke(CHANNELS.modelsRemoveVendor, vendorId) as Promise<void>,
    activeId: () => ipcRenderer.invoke(CHANNELS.modelsActiveId) as Promise<string>,
    setActiveId: (id) => ipcRenderer.invoke(CHANNELS.modelsSetActiveId, id) as Promise<void>,
    vendorsWithKeys: (ids) =>
      ipcRenderer.invoke(CHANNELS.modelsVendorsWithKeys, ids) as Promise<string[]>,
    pickLocal: () => ipcRenderer.invoke(CHANNELS.modelsPickLocal) as Promise<PickedModel | null>,
    loadStates: () =>
      ipcRenderer.invoke(CHANNELS.modelsLoadStates) as Promise<Record<string, ModelLoadState>>,
    offer: () => ipcRenderer.invoke(CHANNELS.modelsOffer) as Promise<LocalOffer>,
    download: () => ipcRenderer.invoke(CHANNELS.modelsDownload) as Promise<boolean>,
    downloadStatus: () =>
      ipcRenderer.invoke(CHANNELS.modelsDownloadStatus) as Promise<ModelDownload>,
    cancelDownload: () => ipcRenderer.invoke(CHANNELS.modelsCancelDownload) as Promise<void>
  },

  keys: {
    save: (vendorId, key) =>
      ipcRenderer.invoke(CHANNELS.keysSave, vendorId, key) as Promise<KeySaveResult>,
    has: (vendorId) => ipcRenderer.invoke(CHANNELS.keysHas, vendorId) as Promise<boolean>,
    remove: (vendorId) => ipcRenderer.invoke(CHANNELS.keysRemove, vendorId) as Promise<void>
  },

  sessions: {
    index: () => ipcRenderer.invoke(CHANNELS.sessionsIndex) as Promise<SessionMeta[]>,
    messages: (id) => ipcRenderer.invoke(CHANNELS.sessionsMessages, id) as Promise<ChatMessage[]>,
    save: (id, messages) =>
      ipcRenderer.invoke(CHANNELS.sessionsSave, id, messages) as Promise<void>,
    remove: (id) => ipcRenderer.invoke(CHANNELS.sessionsRemove, id) as Promise<void>,
    clear: () => ipcRenderer.invoke(CHANNELS.sessionsClear) as Promise<void>
  },

  prefs: {
    get: () => ipcRenderer.invoke(CHANNELS.prefsGet) as Promise<InterfacePrefs>,
    set: (patch) => ipcRenderer.invoke(CHANNELS.prefsSet, patch) as Promise<InterfacePrefs>
  },

  chat: {
    settings: () => ipcRenderer.invoke(CHANNELS.chatSettingsGet) as Promise<ChatSettings>,
    setSettings: (patch) =>
      ipcRenderer.invoke(CHANNELS.chatSettingsSet, patch) as Promise<ChatSettings>,
    start: (providerId: string, messages: WireMessage[], reading: boolean) =>
      ipcRenderer.invoke(CHANNELS.chatStart, providerId, messages, reading) as Promise<number>,
    cancel: (turnId) => ipcRenderer.invoke(CHANNELS.chatCancel, turnId) as Promise<void>,
    forget: () => ipcRenderer.invoke(CHANNELS.chatForget) as Promise<number>,

    onEvent: (listener: (ev: ChatEvent) => void) => {
      // The IpcRendererEvent is dropped rather than forwarded: it carries a
      // `sender`, and handing that to the page would put the whole IPC surface
      // back within reach of the thing contextIsolation exists to fence off.
      const wrapped = (_event: unknown, ev: ChatEvent): void => listener(ev)
      ipcRenderer.on(CHANNELS.chatEvent, wrapped)
      return () => {
        ipcRenderer.removeListener(CHANNELS.chatEvent, wrapped)
      }
    }
  },

  tts: {
    status: () => ipcRenderer.invoke(CHANNELS.ttsStatus) as Promise<TtsStatus>,
    pickModel: (which: TtsSlot) =>
      ipcRenderer.invoke(CHANNELS.ttsPickModel, which) as Promise<PickedModel | null>,
    pickAny: () => ipcRenderer.invoke(CHANNELS.ttsPickAny) as Promise<PickedModel | null>,
    download: () => ipcRenderer.invoke(CHANNELS.ttsDownload) as Promise<boolean>,
    downloadStatus: () => ipcRenderer.invoke(CHANNELS.ttsDownloadStatus) as Promise<ModelDownload>,
    cancelDownload: () => ipcRenderer.invoke(CHANNELS.ttsCancelDownload) as Promise<void>,
    load: () => ipcRenderer.invoke(CHANNELS.ttsLoad) as Promise<string>,
    unload: () => ipcRenderer.invoke(CHANNELS.ttsUnload) as Promise<void>,
    setVoice: (text: string) => ipcRenderer.invoke(CHANNELS.ttsSetVoice, text) as Promise<void>,
    setAutoSpeak: (on: boolean) =>
      ipcRenderer.invoke(CHANNELS.ttsSetAutoSpeak, on) as Promise<void>,
    setSpeaker: (name: string) => ipcRenderer.invoke(CHANNELS.ttsSetSpeaker, name) as Promise<void>,
    setVoiceMode: (mode) => ipcRenderer.invoke(CHANNELS.ttsSetVoiceMode, mode) as Promise<void>,
    setSeed: (seed: number) => ipcRenderer.invoke(CHANNELS.ttsSetSeed, seed) as Promise<void>,
    say: (text: string) => ipcRenderer.invoke(CHANNELS.ttsSay, text) as Promise<boolean>,
    cancel: () => ipcRenderer.invoke(CHANNELS.ttsCancel) as Promise<void>,

    onEvent: (listener: (ev: TtsEvent) => void) => {
      // Same reason as chat's: the IpcRendererEvent carries a `sender`, and the
      // page must not get one.
      const wrapped = (_event: unknown, ev: TtsEvent): void => listener(ev)
      ipcRenderer.on(CHANNELS.ttsEvent, wrapped)
      return () => {
        ipcRenderer.removeListener(CHANNELS.ttsEvent, wrapped)
      }
    }
  },

  omni: {
    status: () => ipcRenderer.invoke(CHANNELS.omniStatus) as Promise<OmniStatus>,
    pickDir: () => ipcRenderer.invoke(CHANNELS.omniPickDir) as Promise<string | null>,
    load: () => ipcRenderer.invoke(CHANNELS.omniLoad) as Promise<string>,
    unload: () => ipcRenderer.invoke(CHANNELS.omniUnload) as Promise<void>,
    set: (patch) => ipcRenderer.invoke(CHANNELS.omniSet, patch) as Promise<OmniStatus>,
    ask: (prompt: string) => ipcRenderer.invoke(CHANNELS.omniAsk, prompt) as Promise<boolean>,

    onEvent: (listener: (ev: OmniEvent) => void) => {
      const wrapped = (_event: unknown, ev: OmniEvent): void => listener(ev)
      ipcRenderer.on(CHANNELS.omniEvent, wrapped)
      return () => {
        ipcRenderer.removeListener(CHANNELS.omniEvent, wrapped)
      }
    }
  },

  reader: {
    status: () => ipcRenderer.invoke(CHANNELS.readerStatus) as Promise<ReaderStatus>,
    load: () => ipcRenderer.invoke(CHANNELS.readerLoad) as Promise<string>,
    unload: () => ipcRenderer.invoke(CHANNELS.readerUnload) as Promise<void>,
    set: (patch) => ipcRenderer.invoke(CHANNELS.readerSet, patch) as Promise<ReaderStatus>,
    pickReference: () =>
      ipcRenderer.invoke(CHANNELS.readerPickReference) as Promise<string | null>,
    useBundled: (name: string) =>
      ipcRenderer.invoke(CHANNELS.readerUseBundled, name) as Promise<string>,
    useRecording: (wav: Uint8Array) =>
      ipcRenderer.invoke(CHANNELS.readerUseRecording, wav) as Promise<string>,
    begin: () => ipcRenderer.invoke(CHANNELS.readerBegin) as Promise<boolean>,
    feed: (delta: string) => ipcRenderer.invoke(CHANNELS.readerFeed, delta) as Promise<boolean>,
    finish: () => ipcRenderer.invoke(CHANNELS.readerFinish) as Promise<void>,
    cancel: () => ipcRenderer.invoke(CHANNELS.readerCancel) as Promise<void>,

    onEvent: (listener: (ev: ReaderEvent) => void) => {
      const wrapped = (_event: unknown, ev: ReaderEvent): void => listener(ev)
      ipcRenderer.on(CHANNELS.readerEvent, wrapped)
      return () => {
        ipcRenderer.removeListener(CHANNELS.readerEvent, wrapped)
      }
    }
  },

  openExternal: (url) => ipcRenderer.invoke(CHANNELS.openExternal, url) as Promise<void>
}

contextBridge.exposeInMainWorld(API_KEY, api)
