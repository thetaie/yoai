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

  chatSettingsGet: 'chat:settings',
  chatSettingsSet: 'chat:setSettings',
  chatStart: 'chat:start',
  chatCancel: 'chat:cancel',
  /** Main -> renderer, one message per streamed event. */
  chatEvent: 'chat:event',

  ttsStatus: 'tts:status',
  ttsPickModel: 'tts:pickModel',
  ttsLoad: 'tts:load',
  ttsUnload: 'tts:unload',
  ttsSetVoice: 'tts:setVoice',
  ttsSetAutoSpeak: 'tts:setAutoSpeak',
  ttsSetSpeaker: 'tts:setSpeaker',
  ttsSetSeed: 'tts:setSeed',
  ttsSay: 'tts:say',
  ttsCancel: 'tts:cancel',
  /** Main -> renderer: PCM chunks and the terminal event. */
  ttsEvent: 'tts:event',

  omniStatus: 'omni:status',
  omniPickDir: 'omni:pickDir',
  omniLoad: 'omni:load',
  omniUnload: 'omni:unload',
  omniSet: 'omni:set',
  omniAsk: 'omni:ask',
  /** Main -> renderer: PCM chunks, then the reply and its timings. */
  omniEvent: 'omni:event',

  openExternal: 'shell:openExternal'
} as const

/** A .gguf the user picked, or null when they dismissed the dialog. */
export interface PickedModel {
  path: string
  /** The file's own name, which is what the picker shows for a local model. */
  name: string
}

/** What speech output can do right now, and why not when it cannot. */
export interface TtsStatus {
  /** False on a build without the qtts lane -- `build.cmd qtts` turns it on. */
  available: boolean
  /** Whether the two GGUFs are resident. Loading takes seconds. */
  loaded: boolean
  /** "base" | "custom_voice" | "voice_design" | '' when nothing is loaded. */
  modelType: string
  /** The stored paths, '' when the user has not picked them yet. */
  talkerPath: string
  tokenizerPath: string
  /**
   * The voice, described in English. On a voice_design checkpoint this is not a
   * preference: bodyclock.h REQUIRES an instruct there, and say() fails without
   * one. Optional on custom_voice, refused by base.
   */
  voice: string
  /** Read every reply aloud, sentence by sentence, as the model writes it. */
  autoSpeak: boolean
  /**
   * The chosen speaker, and the ones this checkpoint carries.
   *
   * Only custom_voice has any. It is the difference that matters when a reply
   * is spoken one sentence at a time: the speaker is a token the engine
   * inserts, so identity is fixed structurally across independent utterances,
   * where a written description only pins it statistically and drifts audibly
   * from sentence to sentence.
   */
  speaker: string
  speakers: string[]
  /**
   * Sampling seed. **Pinned, not left to the engine.**
   *
   * The ABI's default is -1, "a fresh seed per call", and with it the same
   * sentence synthesised twice comes back as different audio -- measured, by
   * hashing the PCM: two runs with everything else held fixed differed, and two
   * with the seed pinned were byte-identical. The speaker holds identity but
   * not the draw, so an unpinned seed is why the test button and a reply did
   * not sound like the same person. -1 restores that behaviour deliberately.
   */
  seed: number
  /** Why the last load or say failed. '' after a success. */
  lastError: string
}

/**
 * One event from a running utterance.
 *
 * `pcm` is **mono float samples at 24 kHz** -- the rate is not carried in the
 * samples, so it is stated here and in native/src/binding.cpp and nowhere else.
 * `stats` is the JSON bodyclock_tts_say returns: speechSeconds, realtime,
 * firstAudioSeconds, chunks, peakDbfs and the rest, documented in bodyclock.h.
 */
export interface TtsEvent {
  type: 'audio' | 'end' | 'error'
  pcm?: Float32Array
  stats?: string
  error?: string
}

/**
 * How the conversation is set up, as opposed to what is said in it.
 *
 * Sent with every turn rather than stored in the transcript: the core keeps no
 * session state, so the prompt in force is always the one configured now, and
 * reopening an old conversation does not resurrect the prompt it was held
 * under.
 */
export interface ChatSettings {
  /** Prepended to every turn as the system message. '' sends none at all. */
  systemPrompt: string
}

/** What the end-to-end lane can do right now. */
export interface OmniStatus {
  /** False on a build without the MNN lane -- `build.cmd clean mnn` turns it on. */
  available: boolean
  loaded: boolean
  /** The model DIRECTORY, not a file: MNN models are a folder of graphs. */
  modelDir: string
  /** Diffusion solver steps; 0 leaves the model's own config alone. */
  ditSteps: number
  /** Route the composer through omni instead of the chat + speech chain. */
  enabled: boolean
}

/** One event from a running omni exchange. Audio is 24 kHz mono, as with TTS. */
export interface OmniEvent {
  type: 'audio' | 'text' | 'end' | 'error'
  pcm?: Float32Array
  /**
   * On 'text': the whole reply, as soon as the thinker stops.
   *
   * It has its own event because 'end' is far too late to show words with:
   * measured here, the text existed at 3.8 s and 'end' arrived at 70.3 s, by
   * which point the speech reading those words had finished playing.
   */
  text?: string
  /** On 'end': the reply text and the timings, as JSON. See bodyclock.h. */
  stats?: string
  error?: string
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
    /** The global system prompt, read fresh at send time. */
    settings(): Promise<ChatSettings>
    setSettings(patch: Partial<ChatSettings>): Promise<ChatSettings>
    start(providerId: string, messages: WireMessage[], systemPrompt: string): Promise<number>
    cancel(turnId: number): Promise<void>
    /** Subscribe to every turn's events. Returns the unsubscribe function. */
    onEvent(listener: (ev: ChatEvent) => void): () => void
  }

  /**
   * Speech output: Qwen3-TTS through qwentts.cpp, in this process.
   *
   * Not a platform voice -- the engine runs here, so `load` costs seconds and
   * gigabytes and `say` costs roughly as long as the speech it produces. The
   * renderer does the playing: `onEvent` hands it PCM and it owns the
   * AudioContext, because only the page has one.
   */
  tts: {
    status(): Promise<TtsStatus>
    /** Pick one of the two GGUFs. `which` says which slot it fills. */
    pickModel(which: 'talker' | 'tokenizer'): Promise<PickedModel | null>
    /** Read both models. Resolves to '' on success, or to the reason. */
    load(): Promise<string>
    unload(): Promise<void>
    /** The written voice; see TtsStatus.voice. Empty restores the default. */
    setVoice(text: string): Promise<void>
    setAutoSpeak(on: boolean): Promise<void>
    /** '' asks the engine for its own default, which is a speaker, not silence. */
    setSpeaker(name: string): Promise<void>
    /** -1 asks for a fresh draw each time; anything else is reproducible. */
    setSeed(seed: number): Promise<void>
    /** False when an utterance is already running. */
    say(text: string): Promise<boolean>
    cancel(): Promise<void>
    /** Subscribe to the running utterance. Returns the unsubscribe function. */
    onEvent(listener: (ev: TtsEvent) => void): () => void
  }

  /**
   * Omni: one model that hears, thinks and speaks.
   *
   * Not a replacement for the chain -- it is the shape the chain cannot be.
   * Costly enough that design/voice.md argues over whether it is affordable;
   * these calls exist so the question can be asked on real hardware.
   */
  omni: {
    status(): Promise<OmniStatus>
    /** Pick the model DIRECTORY. */
    pickDir(): Promise<string | null>
    load(): Promise<string>
    unload(): Promise<void>
    set(patch: { ditSteps?: number; enabled?: boolean }): Promise<OmniStatus>
    /** False when the engine is not loaded or is already answering. */
    ask(prompt: string): Promise<boolean>
    onEvent(listener: (ev: OmniEvent) => void): () => void
  }

  /** Open a vendor's sign-up page in the user's browser, never in a window here. */
  openExternal(url: string): Promise<void>
}
