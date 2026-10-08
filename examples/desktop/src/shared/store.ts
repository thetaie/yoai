/**
 * Shapes the main process persists and the renderer reads back.
 *
 * Here rather than beside the code that writes them, because both sides need
 * them and only the main process may import `electron`. A renderer that pulled
 * these out of src/main would pull the whole file system in with them.
 */

/** A history-drawer row: enough to list and sort, without the transcript. */
export interface SessionMeta {
  id: string
  title: string
  /** Epoch millis; the list sorts on this. */
  updatedAt: number
  /** Messages, shown as a subtitle. */
  count: number
}

/**
 * The three interface preferences, the same three the HarmonyOS app has.
 *
 * `colorMode` is stored as the user's choice, including 'system'. The renderer
 * resolves it against the OS before it reaches the DOM -- see palettes.css for
 * why the stylesheet is not allowed to answer that question itself.
 */
export interface InterfacePrefs {
  colorMode: 'system' | 'light' | 'dark'
  /** Multiplier on the base text size. 1 is the design's own. */
  fontScale: number
  /** One of the three palette families: 'amber', 'arctic', 'quiet'. */
  paletteFamily: string
}

/** Matches design/palettes.json's default, which is the arctic family's light member. */
export const DEFAULT_PREFS: InterfacePrefs = {
  colorMode: 'system',
  fontScale: 1,
  paletteFamily: 'arctic'
}

export interface KeySaveResult {
  ok: boolean
  /** Why not, in a sentence the settings screen can show. Absent on success. */
  reason?: string
}

/** What the main process reports about the addon at startup. */
export interface CoreStatus {
  ok: boolean
  /** The build stamp when ok, the reason it would not load otherwise. */
  detail: string
  /** Whether this build has llama.cpp compiled in. False when the addon is absent. */
  localAvailable: boolean
  /** The OS key-store backend, or '' when keys cannot be stored safely. */
  keyBackend: string
  keysAvailable: boolean
}
