/**
 * The three interface preferences, on disk.
 *
 * The desktop half of examples/harmony/entry/src/main/ets/core/Prefs.ets, and much
 * shorter for a reason that is worth not losing: that file is long because
 * HarmonyOS's PersistentStorage was measured not to read back what it writes,
 * and the comment recording the measurement is most of it. Nothing here shares
 * that platform, so this is an ordinary JSON file.
 *
 * Read before the window is shown. A window created while these are still
 * unknown binds to the default, which is a visible flash of the light theme
 * for someone who chose dark.
 */
import { DEFAULT_PREFS, InterfacePrefs } from '../shared/store'
import { dataFile, readJson, writeJson } from './jsonfile'

const FILE = 'interface.json'

const MODES = new Set(['system', 'light', 'dark'])
const FAMILIES = new Set(['amber', 'arctic', 'quiet'])

/** Font scales the settings screen offers. Clamped, not rounded to these. */
const SCALE_MIN = 0.85
const SCALE_MAX = 1.4

/**
 * Fill in and correct whatever was on disk.
 *
 * Every field is checked rather than trusted: these come out of a file the
 * user can open, and a paletteFamily of "arctik" would otherwise select no
 * CSS block at all and render the app in the browser's own colors.
 */
function sanitize(raw: Partial<InterfacePrefs>): InterfacePrefs {
  const mode = raw.colorMode
  const family = raw.paletteFamily
  const scale = Number(raw.fontScale)

  return {
    colorMode: mode !== undefined && MODES.has(mode) ? mode : DEFAULT_PREFS.colorMode,
    fontScale: Number.isFinite(scale)
      ? Math.min(SCALE_MAX, Math.max(SCALE_MIN, scale))
      : DEFAULT_PREFS.fontScale,
    paletteFamily:
      family !== undefined && FAMILIES.has(family) ? family : DEFAULT_PREFS.paletteFamily
  }
}

export function get(): InterfacePrefs {
  return sanitize(readJson<Partial<InterfacePrefs>>(dataFile(FILE), {}))
}

/** Merge a patch over what is stored, and return the result the app should use. */
export function set(patch: Partial<InterfacePrefs>): InterfacePrefs {
  const next = sanitize({ ...get(), ...patch })
  writeJson(dataFile(FILE), next)
  return next
}
