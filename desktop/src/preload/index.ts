/**
 * The bridge, and the only thing the page can see.
 *
 * Runs in the renderer's process with contextIsolation on, so what is handed to
 * `window.bodyclock` is a plain object of functions and nothing else: no
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
  BodyclockApi,
  CHANNELS,
  ChatSettings,
  PickedModel,
  TtsEvent,
  TtsStatus
} from '../shared/api'
import type { ChatEvent, ChatMessage, WireMessage } from '../shared/chat'
import type { ModelConfig } from '../shared/provider'
import type { CoreStatus, InterfacePrefs, KeySaveResult, SessionMeta } from '../shared/store'

const api: BodyclockApi = {
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
    preload: (id) => ipcRenderer.invoke(CHANNELS.modelsPreload, id) as Promise<string>
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
    start: (providerId: string, messages: WireMessage[], systemPrompt: string) =>
      ipcRenderer.invoke(CHANNELS.chatStart, providerId, messages, systemPrompt) as Promise<number>,
    cancel: (turnId) => ipcRenderer.invoke(CHANNELS.chatCancel, turnId) as Promise<void>,

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
    pickModel: (which: 'talker' | 'tokenizer') =>
      ipcRenderer.invoke(CHANNELS.ttsPickModel, which) as Promise<PickedModel | null>,
    load: () => ipcRenderer.invoke(CHANNELS.ttsLoad) as Promise<string>,
    unload: () => ipcRenderer.invoke(CHANNELS.ttsUnload) as Promise<void>,
    setVoice: (text: string) => ipcRenderer.invoke(CHANNELS.ttsSetVoice, text) as Promise<void>,
    setAutoSpeak: (on: boolean) =>
      ipcRenderer.invoke(CHANNELS.ttsSetAutoSpeak, on) as Promise<void>,
    setSpeaker: (name: string) => ipcRenderer.invoke(CHANNELS.ttsSetSpeaker, name) as Promise<void>,
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

  openExternal: (url) => ipcRenderer.invoke(CHANNELS.openExternal, url) as Promise<void>
}

contextBridge.exposeInMainWorld(API_KEY, api)
