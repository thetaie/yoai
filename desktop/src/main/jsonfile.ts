/**
 * Small JSON files under the app's user-data directory.
 *
 * What `preferences` is to the HarmonyOS app: somewhere to put a handful of
 * settings and a hundred conversations, with no schema and no server. If this
 * ever needs search or real paging the answer is the same as it is there -- a
 * relational store, and the core already carries SQLite for it.
 *
 * Every read is defensive because these files are in a directory the user can
 * open. A truncated or hand-edited file must not stop the app from starting;
 * it must say so and carry on from the default, the way ModelStore.list does.
 */
import { app } from 'electron'
import { mkdirSync, readFileSync, renameSync, unlinkSync, writeFileSync } from 'node:fs'
import { dirname, join } from 'node:path'

/** `<userData>/<name>`, with the directory created. */
export function dataFile(name: string): string {
  const path = join(app.getPath('userData'), name)
  mkdirSync(dirname(path), { recursive: true })
  return path
}

export function readJson<T>(path: string, fallback: T): T {
  let raw: string
  try {
    raw = readFileSync(path, 'utf8')
  } catch {
    return fallback   // absent is the ordinary case on a first run
  }
  try {
    return JSON.parse(raw) as T
  } catch {
    // Corrupt JSON is not worth crashing over, but it IS worth saying out loud:
    // silently returning the default would look identical to a fresh install.
    console.error(`[bodyclock] ${path} is unreadable; starting from the default`)
    return fallback
  }
}

/**
 * Write through a temporary file and rename over the target.
 *
 * A plain write truncates first, so a crash or a full disk between truncate and
 * flush leaves a zero-byte file -- which readJson cannot tell from "never
 * written" and would answer with the default. Losing a session index that way
 * is silent, and rename is atomic on every platform this runs on.
 */
export function writeJson(path: string, value: unknown): void {
  const tmp = path + '.tmp'
  writeFileSync(tmp, JSON.stringify(value, null, 2), 'utf8')
  try {
    renameSync(tmp, path)
  } catch (err) {
    try {
      unlinkSync(tmp)
    } catch {
      // The rename is what failed; the leftover is the smaller problem, and
      // reporting the cleanup instead of the cause would bury it.
    }
    throw err
  }
}
