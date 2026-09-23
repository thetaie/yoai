/**
 * The native addon, loaded and typed.
 *
 * The only file that knows where bodyclock.node is and what its exports look
 * like. Everything else in the main process goes through `core`, so the day the
 * addon grows the voice lane there is one declaration to extend and one loader
 * to leave alone.
 *
 * The addon lives in the MAIN PROCESS and nowhere else. A renderer that could
 * require it would also be a renderer that could read any file the user can,
 * which is the opposite of what contextIsolation is for -- and the key story in
 * the README only holds while the registry is process memory in a process the
 * page cannot reach.
 */
import { createRequire } from 'node:module'
import { existsSync } from 'node:fs'
import { join, resolve } from 'node:path'
import { app } from 'electron'

/** (type, content), the C ABI's own event shape. See bodyclock_chat_handler. */
export type NativeHandler = (type: string, content: string) => void

export interface BodyclockNative {
  nativeVersion(): string
  nativeSetLogLevel(level: string): void

  nativeProviderSet(
    id: string,
    baseUrl: string,
    apiKey: string,
    model: string,
    extraJson: string
  ): number
  nativeProviderRemove(id: string): boolean
  nativeProviderClear(): void
  nativeProviders(): string

  nativeLocalAvailable(): boolean
  nativeLocalSet(id: string, modelPath: string, optionsJson: string): number
  nativeLocalLoad(id: string, onDone: NativeHandler): void
  nativeLocalSetThreads(id: string, threads: number): number
  nativeLocalForget(id: string): number

  nativeChat(
    providerId: string,
    messagesJson: string,
    systemPrompt: string,
    grammar: string,
    onEvent: NativeHandler
  ): number
  nativeChatCancel(turnId: number): void
  nativeChatCancelAll(): number

  //--- Speech output -----------------------------------------------------------
  // Present on every build. The qtts lane is a compile option and the C ABI
  // stubs out without it, so these are always callable and nativeTtsAvailable()
  // is the question to ask.
  nativeTtsAvailable(): number
  // `tokenizerPath` here, `codec_path` in the C ABI: the second file is the
  // 12 Hz audio tokenizer, which is what it is called on disk and in qwentts's
  // own load log. The ABI parameter kept an older name.
  nativeTtsLoad(talkerPath: string, tokenizerPath: string, onDone: NativeHandler): void
  nativeTtsLoaded(): number
  nativeTtsUnload(): void
  nativeTtsModelType(): string
  /** JSON array of speaker names; only CustomVoice checkpoints have any. */
  nativeTtsSpeakers(): string
  nativeTtsSetSpeaker(name: string): void
  nativeTtsSetVoice(instruct: string): void
  nativeTtsSetSeed(seed: number): void
  nativeTtsSetStreaming(on: number): void
  nativeTtsSetThreads(threads: number): void
  /** 1 when the utterance started, 0 when one was already running. */
  nativeTtsSay(text: string, onEvent: NativeTtsHandler): number
  nativeTtsCancel(): number
  nativeTtsLastError(): string

  //--- Reading a reply while it is still being written -------------------------
  // A SECOND engine over the same two GGUFs as the lane above, so only one of
  // them should be loaded: together they hold the weights twice. See
  // src/bodyclock.h and src/voice/reader.hpp.
  nativeReaderAvailable(): number
  nativeReaderLoad(
    talkerPath: string,
    tokenizerPath: string,
    voices: number,
    onDone: NativeHandler
  ): void
  nativeReaderLoaded(): number
  nativeReaderUnload(): void
  nativeReaderModelType(): string
  nativeReaderSetVoice(instruct: string): void
  nativeReaderSetSpeaker(name: string): void
  /** '' on success, else the reason. Base checkpoints only; runs the speaker encoder. */
  nativeReaderSetReferenceVoice(wavPath: string): string
  nativeReaderSetSeed(seed: number): void
  /** A field <= 0 is left at the measured default. */
  nativeReaderSetSplit(minChars: number, maxChars: number, tailChars: number): void
  /** A comma ends a piece once min_chars is reached. Off by default. */
  nativeReaderSetClauseCuts(on: boolean): void
  nativeReaderSetThreads(threads: number): void
  /** 1 when the stream opened, 0 when one was already open. */
  nativeReaderBegin(onAudio: NativeReaderHandler): number
  nativeReaderFeed(delta: string): number
  /** Returns at once; onDone fires when the last sample has gone out. */
  nativeReaderFinish(onDone: NativeHandler): void
  nativeReaderLastError(): string

  //--- Omni (MNN + Qwen2.5-Omni) -----------------------------------------------
  // Present on every build, stubbed without the lane, exactly like the TTS
  // entries above.
  nativeOmniAvailable(): number
  /** A DIRECTORY: config.json plus a dozen .mnn graphs, about 3.8 GB. */
  nativeOmniLoad(modelDir: string, onDone: NativeHandler): void
  nativeOmniLoaded(): number
  nativeOmniUnload(): void
  /** MNN runs `steps - 1` diffusion forwards per chunk. 0 on success. */
  nativeOmniSetDitSteps(steps: number): number
  /** pcm is 16-bit mono at 16 kHz. 1 when the exchange started. */
  nativeOmniAsk(
    pcm: Int16Array,
    prompt: string,
    wantAudio: boolean,
    onEvent: NativeTtsHandler
  ): number
}

/**
 * (type, payload) for speech output. `payload` is a Float32Array of mono 24 kHz
 * samples when type is 'audio', and a string otherwise -- the stats JSON on
 * 'end', the reason on 'error'. One callback rather than two because the events
 * are ordered and splitting them would let the terminal one overtake a chunk.
 */
export type NativeTtsHandler = (type: string, payload: Float32Array | string) => void

/**
 * The reading lane's audio handler.
 *
 * A third argument the other lanes do not have: which PIECE these samples
 * belong to. The binding passes it only for this lane, so the speech and omni
 * callbacks keep the arity they were written against.
 */
export type NativeReaderHandler = (
  type: string,
  payload: Float32Array | string,
  sentence?: number
) => void

/**
 * Where the addon might be, most specific first.
 *
 * Searched rather than computed because the answer differs three ways and only
 * one of them is under this file's control: cmake-js puts the artefact under
 * build/ with a layout that depends on the generator (Ninja writes one
 * directory, Visual Studio a per-configuration one), and a packaged app has it
 * beside the asar instead. The override comes first so a bisect can point at a
 * specific build without moving files around.
 */
function candidates(): string[] {
  const out: string[] = []

  const override = process.env.BODYCLOCK_ADDON
  if (override !== undefined && override.length > 0) {
    out.push(resolve(override))
  }

  if (app.isPackaged) {
    out.push(join(process.resourcesPath, 'bodyclock.node'))
  }

  // app.getAppPath() is desktop/ in development and the asar in a packaged app;
  // the build tree only exists in the first case, and the packaged path above
  // has already been tried by then.
  const root = app.getAppPath()
  out.push(join(root, 'build', 'Release', 'bodyclock.node'))
  out.push(join(root, 'build', 'bodyclock.node'))
  out.push(join(root, 'build', 'Debug', 'bodyclock.node'))

  return out
}

let cached: BodyclockNative | null = null

/**
 * The addon, loaded on first use.
 *
 * Throws with the list of paths it tried, because the only two reasons it is
 * missing are "not built yet" and "built somewhere else", and the paths tell
 * those apart without a second run.
 */
export function core(): BodyclockNative {
  if (cached !== null) {
    return cached
  }

  const tried = candidates()
  const found = tried.find((p) => existsSync(p))
  if (found === undefined) {
    throw new Error(
      'bodyclock.node was not found. Build it with `npm run build:native` ' +
        '(or set BODYCLOCK_ADDON). Looked in:\n  ' +
        tried.join('\n  ')
    )
  }

  const requireNative = createRequire(__filename)
  cached = requireNative(found) as BodyclockNative
  return cached
}

/**
 * Load the addon and say what it is, or explain why it would not load.
 *
 * Called once at startup so a broken build is reported while the window is
 * still blank, rather than as a failed first message.
 */
export function describeCore(): { ok: true; version: string } | { ok: false; error: string } {
  try {
    return { ok: true, version: core().nativeVersion() }
  } catch (err) {
    return { ok: false, error: err instanceof Error ? err.message : String(err) }
  }
}
