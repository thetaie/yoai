/**
 * The user's configured models, and the one they are chatting with.
 *
 * A port of harmony/entry/src/main/ets/core/ModelStore.ets, including the split
 * it is built on: the non-secret part (vendor, base URL, model name, which one
 * is selected) lives in a JSON file, and the key lives in [keystore]. Nothing
 * here ever holds a key in a field -- it is read at the moment it is handed to
 * the core, and not kept.
 *
 * The core's registry is process memory, so it is empty after every cold start.
 * syncToCore() is what refills it, and is the reason this module knows about
 * the addon at all.
 */
import { existsSync } from 'node:fs'
import { ModelConfig, isLocal } from '../shared/provider'
import { core } from './core'
import { dataFile, readJson, writeJson } from './jsonfile'
import { getKey, hasKey } from './keystore'

interface Stored {
  configured: ModelConfig[]
  active: string
}

const FILE = 'models.json'
const EMPTY: Stored = { configured: [], active: '' }

function read(): Stored {
  const s = readJson<Stored>(dataFile(FILE), EMPTY)
  // A file that parsed but is not the shape we wrote: same treatment as one
  // that did not parse, because the caller cannot iterate either of them.
  if (!Array.isArray(s.configured)) {
    return EMPTY
  }
  // `active` is normalised to a string here so every caller can treat it as
  // one. A file written before this field existed parses fine and leaves it
  // undefined, and the difference only shows up at the one call site that
  // reaches for .length.
  return { configured: s.configured, active: typeof s.active === 'string' ? s.active : '' }
}

function write(s: Stored): void {
  writeJson(dataFile(FILE), s)
}

/** Every model the user has configured, in the order they added them. */
export function list(): ModelConfig[] {
  return read().configured
}

/**
 * Add or replace one model, and register it with the core so it is usable
 * immediately -- without this the user would have to restart the app after
 * adding a model, which is the kind of thing nobody reports as a bug because
 * they assume it is how the app works.
 */
export function add(cfg: ModelConfig): void {
  const s = read()
  s.configured = s.configured.filter((m) => m.id !== cfg.id).concat(cfg)
  // Selecting it when nothing is selected. remove() has always repaired a
  // dangling `active`; nothing repaired an EMPTY one, so the first model ever
  // added left it '' and the app then reported having no model while the picker
  // displayed that model's name -- a `<select>` whose value matches no option
  // renders the first one, so the list looked chosen and was not.
  //
  // Only when empty. Adding a second model while one is selected must not
  // silently move the conversation to it.
  if (s.active.length === 0) {
    s.active = cfg.id
  }
  write(s)
  register(cfg)
}

/** Forget one model. Its vendor's key is left alone -- other models may share it. */
export function remove(id: string): void {
  const s = read()
  s.configured = s.configured.filter((m) => m.id !== id)

  // The selection may have just pointed at it.
  if (s.active === id) {
    s.active = s.configured.length > 0 ? s.configured[s.configured.length - 1].id : ''
  }
  write(s)
  core().nativeProviderRemove(id)
}

/**
 * Drop every model belonging to a vendor, and that vendor's key. What the
 * settings screen calls "remove this service": leaving models behind whose key
 * is gone would list entries that can only fail.
 *
 * The key is deleted by the caller in ipc.ts rather than here, so this module
 * stays the one that owns the list and keystore stays the one that owns keys.
 */
export function removeVendor(vendorId: string): void {
  for (const m of list()) {
    if (m.vendorId === vendorId) {
      remove(m.id)
    }
  }
}

/** The model the composer is pointed at, or '' when nothing is configured. */
export function activeId(): string {
  return read().active
}

export function setActiveId(id: string): void {
  const s = read()
  s.active = id
  write(s)
}

/**
 * Register one model with the core. For a remote model that means reading its
 * key from the key store; for an on-device one, handing over the file path.
 *
 * Returns false when the model cannot run -- no key for its vendor, or no file.
 * The core is then left WITHOUT the entry, deliberately: bodyclock_providers()
 * is the picker's source of truth, so an entry that cannot run must not appear
 * in it.
 */
export function register(cfg: ModelConfig): boolean {
  if (isLocal(cfg)) {
    const path = cfg.modelPath
    if (path === undefined || path.length === 0) {
      return false
    }
    // '' for the options: the core's defaults are documented in src/bodyclock.h
    // and were tuned on a phone. They are a reasonable desktop starting point
    // and there is no settings screen for them yet; when there is, it goes here.
    const rc = core().nativeLocalSet(cfg.id, path, '')
    if (rc !== 0) {
      console.error(`[bodyclock] nativeLocalSet(${cfg.id}) failed rc=${rc}`)
      return false
    }
    return true
  }

  const key = getKey(cfg.vendorId)
  if (key.length === 0) {
    return false
  }
  const rc = core().nativeProviderSet(cfg.id, cfg.baseUrl, key, cfg.model, '')
  if (rc !== 0) {
    console.error(`[bodyclock] nativeProviderSet(${cfg.id}) failed rc=${rc}`)
    return false
  }
  return true
}

/**
 * Refill the core's registry from what is stored. Call once at startup, before
 * the first turn. Returns how many models are actually usable.
 *
 * On-device entries are checked FIRST: a .gguf the user moved or deleted would
 * otherwise sit in the picker and fail only when someone tried to use it. Ones
 * whose file has gone are dropped from the stored list here -- the single place
 * that owns that list, so an entry cannot vanish from two directions at once.
 *
 * This is the desktop half of what LocalModelStore.activate does on HarmonyOS.
 * There the question is whether a picker grant survived the restart; here a
 * path is just a path, so the question is only whether the file is still there.
 */
export function syncToCore(): number {
  core().nativeProviderClear()

  const s = read()

  const gone = new Set(
    s.configured
      .filter((m) => isLocal(m) && (m.modelPath === undefined || !existsSync(m.modelPath)))
      .map((m) => m.id)
  )
  if (gone.size > 0) {
    console.warn(`[bodyclock] dropping ${gone.size} unreachable on-device model(s)`)
    s.configured = s.configured.filter((m) => !gone.has(m.id))
    if (gone.has(s.active)) {
      s.active = s.configured.length > 0 ? s.configured[s.configured.length - 1].id : ''
    }
    write(s)
  }

  // Heal a selection that never got made. add() sets it now, but a store
  // written before it did has models and an empty `active`, and nothing else
  // would ever fill it: the picker only writes a selection when the user
  // changes it, and they cannot change it to the value it already displays.
  if (s.active.length === 0 && s.configured.length > 0) {
    s.active = s.configured[s.configured.length - 1].id
    write(s)
  }

  let ready = 0
  for (const m of s.configured) {
    if (register(m)) {
      ready++
    }
  }
  console.log(`[bodyclock] registered ${ready} of ${s.configured.length} models`)
  return ready
}

/**
 * Read the selected on-device model's weights now, without being asked.
 *
 * Only the ACTIVE one. Every configured local model is gigabytes, and loading
 * a model nobody selected would spend the memory of a conversation that is not
 * happening. The first turn pays for the weights otherwise, which is the one
 * moment the user is already waiting.
 *
 * Safe to run behind the window: bodyclock_local_load takes no lock the rest of
 * the ABI needs -- unlike the TTS one -- and the addon runs it on a worker
 * thread. Nothing waits on the result; it is logged and dropped.
 */
/**
 * Forget every on-device model's conversation. The weights stay resident.
 *
 * Called when a new conversation starts, and not once per turn: a turn already
 * carries its whole transcript, so the only moment the cache holds something the
 * next prompt has no business seeing is this one.
 *
 * All of them rather than the active one, because switching models is a way to
 * leave a conversation behind too, and the one being left is the one that still
 * has it. Each call is a cache drop against a model that is loaded, or a
 * no-op-with-a-warning against one that is not.
 */
export function forgetAll(): number {
  let cleared = 0
  for (const cfg of list()) {
    if (isLocal(cfg) && core().nativeLocalForget(cfg.id) === 0) {
      cleared++
    }
  }
  return cleared
}

export function autoPreload(): void {
  const s = read()
  const cfg = s.configured.find((m) => m.id === s.active)
  if (cfg === undefined || !isLocal(cfg)) {
    return
  }
  console.log(`[bodyclock] preloading ${cfg.id}`)
  core().nativeLocalLoad(cfg.id, (type, content) => {
    if (type === 'end') {
      console.log(`[bodyclock] ${cfg.id} is resident`)
    } else {
      console.error(`[bodyclock] ${cfg.id} would not preload: ${content}`)
    }
  })
}

/** Which vendors have a key stored. Drives the settings screen's state. */
export function vendorsWithKeys(vendorIds: string[]): string[] {
  return vendorIds.filter(hasKey)
}
