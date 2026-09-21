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

  nativeChat(
    providerId: string,
    messagesJson: string,
    systemPrompt: string,
    grammar: string,
    onEvent: NativeHandler
  ): number
  nativeChatCancel(turnId: number): void
  nativeChatCancelAll(): number
}

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
