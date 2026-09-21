/**
 * The contract between the renderer and the main process.
 *
 * This is the WHOLE surface the page can reach. The renderer has no Node, no
 * file system and no addon; it has these calls, and each one is a named handler
 * in src/main/ipc.ts. Adding a capability means adding it here first, which is
 * the point -- a bridge that grew by `ipcRenderer.invoke(anything)` would be a
 * bridge with no surface to review.
 *
 * Read alongside src/bodyclock.h: `chat` and `models` are that header's
 * vocabulary with the blocking parts moved off the drawing thread, and nothing
 * in between reinterprets an event.
 */
import type { ChatEvent, ChatMessage, WireMessage } from './chat'
import type { ModelConfig } from './provider'
import type { CoreStatus, InterfacePrefs, KeySaveResult, SessionMeta } from './store'

/** The name the preload script binds the API to on `window`. */
export const API_KEY = 'bodyclock'

/** IPC channel names, in one place so the two sides cannot drift apart. */
export const CHANNELS = {
  coreStatus: 'core:status',

  modelsList: 'models:list',
  modelsAdd: 'models:add',
  modelsRemove: 'models:remove',
  modelsRemoveVendor: 'models:removeVendor',
  modelsActiveId: 'models:activeId',
  modelsSetActiveId: 'models:setActiveId',
  modelsVendorsWithKeys: 'models:vendorsWithKeys',
  modelsPickLocal: 'models:pickLocal',
  modelsPreload: 'models:preload',

  keysSave: 'keys:save',
  keysHas: 'keys:has',
  keysRemove: 'keys:remove',

  sessionsIndex: 'sessions:index',
  sessionsMessages: 'sessions:messages',
  sessionsSave: 'sessions:save',
  sessionsRemove: 'sessions:remove',
  sessionsClear: 'sessions:clear',

  prefsGet: 'prefs:get',
  prefsSet: 'prefs:set',

  chatStart: 'chat:start',
  chatCancel: 'chat:cancel',
  /** Main -> renderer, one message per streamed event. */
  chatEvent: 'chat:event',

  openExternal: 'shell:openExternal'
} as const

/** A .gguf the user picked, or null when they dismissed the dialog. */
export interface PickedModel {
  path: string
  /** The file's own name, which is what the picker shows for a local model. */
  name: string
}

export interface BodyclockApi {
  /** Whether the addon loaded, and what it can do. Read once at startup. */
  coreStatus(): Promise<CoreStatus>

  models: {
    list(): Promise<ModelConfig[]>
    add(cfg: ModelConfig): Promise<void>
    remove(id: string): Promise<void>
    /** Drops the vendor's models AND its key. */
    removeVendor(vendorId: string): Promise<void>
    activeId(): Promise<string>
    setActiveId(id: string): Promise<void>
    vendorsWithKeys(vendorIds: string[]): Promise<string[]>
    pickLocal(): Promise<PickedModel | null>
    /**
     * Read an on-device model's weights now, so the first turn does not pay for
     * them. Resolves to '' when the model is resident, or to the reason it is
     * not. Takes seconds and gigabytes; call it while the user is still typing.
     */
    preload(id: string): Promise<string>
  }

  keys: {
    /** The key goes to the main process and no further. */
    save(vendorId: string, key: string): Promise<KeySaveResult>
    has(vendorId: string): Promise<boolean>
    remove(vendorId: string): Promise<void>
  }

  sessions: {
    index(): Promise<SessionMeta[]>
    messages(id: string): Promise<ChatMessage[]>
    save(id: string, messages: ChatMessage[]): Promise<void>
    remove(id: string): Promise<void>
    clear(): Promise<void>
  }

  prefs: {
    get(): Promise<InterfacePrefs>
    /** Returns the merged result, already corrected. */
    set(patch: Partial<InterfacePrefs>): Promise<InterfacePrefs>
  }

  chat: {
    /**
     * Start one turn and resolve to its id. `messages` is the WHOLE transcript
     * including the user message just sent -- the core keeps no session state.
     */
    start(providerId: string, messages: WireMessage[], systemPrompt: string): Promise<number>
    cancel(turnId: number): Promise<void>
    /** Subscribe to every turn's events. Returns the unsubscribe function. */
    onEvent(listener: (ev: ChatEvent) => void): () => void
  }

  /** Open a vendor's sign-up page in the user's browser, never in a window here. */
  openExternal(url: string): Promise<void>
}
