/**
 * API keys, on this machine only.
 *
 * The hard rule this file exists to hold, the same one
 * harmony/entry/src/main/ets/core/SecureKeyStore.ets holds: a key the user
 * pastes is encrypted by the operating system and nowhere else. It is never
 * written in the clear, never logged, never put in a crash report, and there is
 * no Body Clock server for it to be sent to.
 *
 * Backed by Electron's safeStorage -- DPAPI on Windows, the Keychain on macOS,
 * the desktop's own secret service on Linux -- with the ciphertext kept in
 * <userData>/keys.json, one base64 blob per vendor.
 *
 * **Refusing to save is a feature here.** On a Linux box with no keyring
 * safeStorage still "works": it falls back to a built-in password, which is
 * encryption that anyone with the file can undo. Writing a key under that and
 * calling it stored would make this file's one promise false, so it reports the
 * backend and declines. The nearest honest alternative is the environment
 * variable the CLI already takes.
 */
import { safeStorage } from 'electron'
import { KeySaveResult } from '../shared/store'
import { dataFile, readJson, writeJson } from './jsonfile'

/** vendorId -> base64 of the OS-encrypted key. */
type Vault = Record<string, string>

const FILE = 'keys.json'

/** Backends that are not encryption, whatever the API calls them. */
const WEAK_BACKENDS = new Set(['basic_text', 'unknown'])

function vault(): Vault {
  return readJson<Vault>(dataFile(FILE), {})
}

/**
 * The OS backend's name, or '' where the platform does not report one.
 *
 * getSelectedStorageBackend is Linux-only and throws elsewhere, which is fine:
 * Windows and macOS have exactly one backend each and it is a real one.
 */
export function backend(): string {
  if (process.platform !== 'linux') {
    return process.platform === 'win32' ? 'dpapi' : 'keychain'
  }
  try {
    return safeStorage.getSelectedStorageBackend()
  } catch {
    return 'unknown'
  }
}

/** Whether a key saved right now would actually be encrypted. */
export function available(): boolean {
  if (!safeStorage.isEncryptionAvailable()) {
    return false
  }
  return !WEAK_BACKENDS.has(backend())
}

/** Store (or replace) the API key for a vendor. */
export function saveKey(vendorId: string, key: string): KeySaveResult {
  if (key.length === 0) {
    return { ok: false, reason: 'the key is empty' }
  }
  if (!available()) {
    return {
      ok: false,
      reason:
        `this system has no encrypted key store (backend: ${backend() || 'none'}), ` +
        'so the key would be saved in a form anyone with the file could read'
    }
  }

  const v = vault()
  v[vendorId] = safeStorage.encryptString(key).toString('base64')
  writeJson(dataFile(FILE), v)
  return { ok: true }
}

/**
 * The stored key for a vendor, or '' when there is none.
 *
 * Main process only, and never sent over IPC: the renderer asks whether a key
 * EXISTS (hasKey) and the key itself travels from here straight into the core's
 * registry. A key that reaches a web page is a key in a heap dump.
 */
export function getKey(vendorId: string): string {
  const blob = vault()[vendorId]
  if (blob === undefined || blob.length === 0) {
    return ''
  }
  try {
    return safeStorage.decryptString(Buffer.from(blob, 'base64'))
  } catch {
    // The usual cause is a profile copied to another machine or user account,
    // where the OS will not undo its own encryption. Treat it as absent -- the
    // settings screen then offers to paste it again, which is the fix.
    console.error(`[bodyclock] the stored key for ${vendorId} could not be decrypted`)
    return ''
  }
}

export function hasKey(vendorId: string): boolean {
  return getKey(vendorId).length > 0
}

/** Remove the key for a vendor; a no-op when there is none. */
export function deleteKey(vendorId: string): void {
  const v = vault()
  if (!(vendorId in v)) {
    return
  }
  delete v[vendorId]
  writeJson(dataFile(FILE), v)
}
