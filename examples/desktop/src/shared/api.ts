/**
 * The contract between the renderer and the main process.
 *
 * This is the WHOLE surface the page can reach. The renderer has no Node, no
 * file system and no addon; it has these calls, and each one is a named handler
 * in src/main/ipc.ts. Adding a capability means adding it here first, which is
 * the point -- a bridge that grew by `ipcRenderer.invoke(anything)` would be a
 * bridge with no surface to review.
 *
 * Read alongside src/yoai.h: `chat` and `models` are that header's
 * vocabulary with the blocking parts moved off the drawing thread, and nothing
 * in between reinterprets an event.
 */
import type { ChatEvent, ChatMessage, WireMessage } from './chat'
import type { ModelConfig } from './provider'
import type { CoreStatus, InterfacePrefs, KeySaveResult, SessionMeta } from './store'

/** The name the preload script binds the API to on `window`. */
export const API_KEY = 'yoai'

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
  modelsLoadStates: 'models:loadStates',

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
  /** A new conversation: drop what the on-device engines still hold. */
  chatForget: 'chat:forget',
  /** Main -> renderer, one message per streamed event. */
  chatEvent: 'chat:event',

  ttsStatus: 'tts:status',
  ttsPickModel: 'tts:pickModel',
  ttsPickAny: 'tts:pickAny',
  ttsDownload: 'tts:download',
  ttsDownloadStatus: 'tts:downloadStatus',
  ttsCancelDownload: 'tts:cancelDownload',
  ttsLoad: 'tts:load',
  ttsUnload: 'tts:unload',
  ttsSetVoice: 'tts:setVoice',
  ttsSetAutoSpeak: 'tts:setAutoSpeak',
  ttsSetSpeaker: 'tts:setSpeaker',
  ttsSetSeed: 'tts:setSeed',
  ttsSetVoiceMode: 'tts:setVoiceMode',
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

  readerStatus: 'reader:status',
  readerLoad: 'reader:load',
  readerUnload: 'reader:unload',
  readerSet: 'reader:set',
  readerBegin: 'reader:begin',
  readerFeed: 'reader:feed',
  readerFinish: 'reader:finish',
  readerCancel: 'reader:cancel',
  readerPickReference: 'reader:pickReference',
  readerUseBundled: 'reader:useBundled',
  readerUseRecording: 'reader:useRecording',
  /** Main -> renderer: PCM chunks in reading order, then the timings. */
  readerEvent: 'reader:event',

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
  /** False on a build without the qtts lane -- `build.py qtts` turns it on. */
  available: boolean
  /** Whether the two GGUFs are resident. Loading takes seconds. */
  loaded: boolean
  /** "base" | "custom_voice" | "voice_design" | '' when nothing is loaded. */
  modelType: string
  /**
   * Where the voice comes from: 'base', a reference recording through a Base
   * talker, or 'custom', a speaker built into a CustomVoice talker, which
   * also takes a style (ReaderStatus.style). Each has its own talker file.
   */
  voiceMode: VoiceMode
  /** The stored paths, '' when the user has not picked them yet. */
  talkerPath: string
  /** The CustomVoice talker, for voiceMode 'custom'. */
  customTalkerPath: string
  tokenizerPath: string
  /**
   * The voice, described in English. On a voice_design checkpoint this is not a
   * preference: yoai.h REQUIRES an instruct there, and say() fails without
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
  /**
   * The files the current voice mode needs and does not have -- never picked,
   * or picked and since gone -- each with the file a download would fetch.
   * What the settings screen offers to pick or download; [] when both are there.
   */
  missing: TtsMissing[]
}

/** A model slot: which stored path a file fills. */
export type TtsSlot = 'talker' | 'customTalker' | 'tokenizer'

/** One file a voice mode is missing, and what a download would fetch for it. */
export interface TtsMissing {
  slot: TtsSlot
  name: string
  /** Bytes. */
  size: number
}

/**
 * A download of the speech models into a folder the user chose. It runs in the
 * main process, so it carries on while the settings screen is closed.
 */
export interface TtsDownload {
  running: boolean
  /** The folder being downloaded into. */
  dir: string
  /** The file being fetched or checked now. */
  file: string
  /** huggingface.co or hf-mirror.com, whichever answered first; '' until one has. */
  host: string
  /** Bytes on disk so far and in all, across every file this download fetches. */
  received: number
  total: number
  /** Checking `file` against its SHA-256, which takes seconds. */
  verifying: boolean
  /**
   * Waiting to retry after the connection broke: which retry comes next,
   * 1-based, of `retries`, and in how many seconds. `retry` is 0 otherwise.
   */
  retry: number
  retries: number
  retryIn: number
  /**
   * Why the last download stopped, '' when it did not fail. 'space': not enough
   * room in `dir`. 'network': no host answered, or the transfer broke off and
   * the retries did not bring it back -- downloading again resumes it.
   * 'checksum': the file was not the one expected and has been deleted.
   * 'disk': `dir` could not be written. 'canceled': asked to stop.
   */
  error: '' | 'space' | 'network' | 'checksum' | 'disk' | 'canceled'
  /** The underlying message behind `error`, in English; '' when there is none. */
  detail: string
}

/**
 * One event from a running utterance.
 *
 * `pcm` is **mono float samples at 24 kHz** -- the rate is not carried in the
 * samples, so it is stated here and in native/src/binding.cpp and nowhere else.
 * `stats` is the JSON yoai_tts_say returns: speechSeconds, realtime,
 * firstAudioSeconds, chunks, peakDbfs and the rest, documented in yoai.h.
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
  /** False on a build without the MNN lane -- `build.py clean mnn` turns it on. */
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
  /** On 'end': the reply text and the timings, as JSON. See yoai.h. */
  stats?: string
  error?: string
}

/** An on-device model's weights: being read, resident, or refused with a reason. */
export interface ModelLoadState {
  state: 'loading' | 'ready' | 'failed'
  /** Why it failed; '' otherwise. */
  reason: string
}

/** Which talker, and so which kind of voice; see TtsStatus.voiceMode. */
export type VoiceMode = 'base' | 'custom'

/**
 * How a CustomVoice talker is asked to read: '' for no instruction, or one of
 * a few short presets. The wording of each is in main/reader.ts.
 */
export type ReaderStyle = '' | 'calm' | 'caring' | 'cheerful' | 'sorry'

/** What the reading lane can do right now. */
export interface ReaderStatus {
  available: boolean
  loaded: boolean
  /** "voice_design", "base" or "custom_voice"; '' before a load. */
  modelType: string
  /**
   * The .spk every piece is anchored to; '' when none -- nothing loaded, or not
   * a Base checkpoint. Picked from speaker/ by the talker's size at load.
   *
   * Without one the pieces drift in register -- one sentence in sixteen came
   * out five semitones low, heard as the voice changing mid-reply.
   */
  speaker: string
  /** The reference voice the user picked, a .wav or a .spk; '' for a bundled one. */
  reference: string
  /** The bundled voice used while `reference` is '': a name from bundledVoices. */
  bundled: string
  /** The voices that come with the app, the directories under speaker/. */
  bundledVoices: string[]
  /**
   * What went wrong with the last automatic load, '' when nothing did -- a
   * talker that is not a Base checkpoint, above all, which is refused.
   */
  note: string
  /** 0 means the core's measured default of 30. */
  minChars: number
  /** The talker's sampling temperature: higher reads with more varied pace. */
  temperature: number
  /** Read every reply aloud as it is written, without being asked. */
  enabled: boolean
  /** The named speakers of a CustomVoice talker; [] on any other. */
  speakers: string[]
  /** The style a CustomVoice talker reads in. */
  style: ReaderStyle
}

/** One event from a reply being read as it is written. Audio is 24 kHz mono. */
export interface ReaderEvent {
  type: 'audio' | 'end' | 'error'
  pcm?: Float32Array
  /** On 'audio': which piece these samples belong to, so seams can be logged. */
  sentence?: number
  /** On 'end': the timings, as JSON. See yoai.h. */
  stats?: string
  error?: string
}

export interface YoaiApi {
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
     * How the automatic preload of each on-device model went, by model id. Only
     * models a preload was started for appear: the active one, at startup and
     * when it is switched to.
     */
    loadStates(): Promise<Record<string, ModelLoadState>>
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
    /**
     * Start a new conversation, as far as the engines are concerned: drop what
     * the on-device ones still hold. Resolves to how many were cleared.
     *
     * A turn replays the whole transcript, so this is not what makes the next
     * prompt correct -- it is what stops the last conversation being in the
     * cache underneath it. See src/yoai.h on yoai_local_forget.
     */
    forget(): Promise<number>
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
    /**
     * Pick a GGUF. `which` says which slot it fills; empty slots are filled
     * from files beside it whose names say what they are.
     */
    pickModel(which: TtsSlot): Promise<PickedModel | null>
    /**
     * Pick any one of the model files. Its slot is read from its name, else it
     * fills the first missing one, and the other empty slots are filled from
     * beside it as with pickModel.
     */
    pickAny(): Promise<PickedModel | null>
    /**
     * Choose a folder and download into it what TtsStatus.missing lists,
     * skipping any file already there. Resolves once it has started: false
     * when the dialog was dismissed, nothing is missing or one is running.
     * The voice loads by itself when it is done.
     */
    download(): Promise<boolean>
    downloadStatus(): Promise<TtsDownload>
    /** Stop the running download and delete the file it was part way through. */
    cancelDownload(): Promise<void>
    /** Base or CustomVoice; the voice is reloaded with the other talker. */
    setVoiceMode(mode: VoiceMode): Promise<void>
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

  /**
   * Reading a reply aloud while the model is still writing it.
   *
   * A second engine over the speech lane's own two GGUFs, so loading this one
   * unloads that one. What it buys is the seconds a single utterance cannot
   * recover: the text is what the engine would otherwise be waiting for.
   */
  reader: {
    status(): Promise<ReaderStatus>
    load(): Promise<string>
    unload(): Promise<void>
    set(patch: {
      enabled?: boolean
      minChars?: number
      temperature?: number
      style?: ReaderStyle
    }): Promise<ReaderStatus>
    /**
     * Pick a reference voice, a .wav or a .spk, and use it at once. null when
     * the dialog was dismissed, else '' or why the voice could not be used. A
     * recording takes seconds the first time, while its embedding is extracted.
     */
    pickReference(): Promise<string | null>
    /**
     * Use a bundled voice, by a name from ReaderStatus.bundledVoices, in place
     * of any file picked. '' or why it could not be used.
     */
    useBundled(name: string): Promise<string>
    /**
     * Keep a take recorded on this page -- a 16-bit mono WAV at 24 kHz -- and use
     * it as the reference voice. '' or why it could not be used.
     */
    useRecording(wav: Uint8Array): Promise<string>
    /** Open a stream. False when the engine is not loaded or one is open. */
    begin(): Promise<boolean>
    /** Add text. Returns at once; the audio arrives through onEvent. */
    feed(delta: string): Promise<boolean>
    /** No more text. The 'end' event follows once the last sample is out. */
    finish(): Promise<void>
    /** Abandon the open stream. It must still be finished to be closed. */
    cancel(): Promise<void>
    onEvent(listener: (ev: ReaderEvent) => void): () => void
  }

  /** Open a vendor's sign-up page in the user's browser, never in a window here. */
  openExternal(url: string): Promise<void>
}
