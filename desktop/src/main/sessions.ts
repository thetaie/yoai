/**
 * Locally persisted chat sessions -- what the history drawer lists.
 *
 * A port of harmony/entry/src/main/ets/core/SessionStore.ets. There is nothing
 * to sync with: Body Clock talks straight to the provider the user configured
 * and has no account, so history is purely local and stays that way.
 *
 * Storage layout: <userData>/sessions/index.json holds the INDEX (ids, titles,
 * timestamps -- small), and each session's messages are a file of their own.
 * Split for the same two reasons as on the phone: the drawer needs only the
 * index to render, so opening it never reads megabytes of transcript, and one
 * huge conversation cannot slow down every other session's read.
 */
import { existsSync, mkdirSync, readdirSync, rmSync } from 'node:fs'
import { join } from 'node:path'
import { app } from 'electron'
import { ChatMessage } from '../shared/chat'
import { SessionMeta } from '../shared/store'
import { readJson, writeJson } from './jsonfile'

/** Sessions past this many are dropped, oldest first, when a new one is saved. */
const MAX_SESSIONS = 100

/** Characters of the first user message kept as the session title. */
const TITLE_LEN = 60

function dir(): string {
  const path = join(app.getPath('userData'), 'sessions')
  mkdirSync(path, { recursive: true })
  return path
}

/**
 * A session id is part of a FILE NAME, so it is checked rather than trusted.
 *
 * Ids are minted in the renderer, which is a web page: the one thing it must
 * not be able to do is name a path. Anything but the generator's own alphabet
 * is refused, so '../../keys' is a rejected id and not a written file.
 */
function idPath(id: string): string {
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(id)) {
    throw new Error(`refusing a session id that is not a plain token: ${JSON.stringify(id)}`)
  }
  return join(dir(), id + '.json')
}

function indexPath(): string {
  return join(dir(), 'index.json')
}

/** The drawer's list, newest first. */
export function index(): SessionMeta[] {
  const list = readJson<SessionMeta[]>(indexPath(), [])
  if (!Array.isArray(list)) {
    return []
  }
  return [...list].sort((a, b) => b.updatedAt - a.updatedAt)
}

function writeIndex(list: SessionMeta[]): void {
  writeJson(indexPath(), list)
}

export function messages(id: string): ChatMessage[] {
  const list = readJson<ChatMessage[]>(idPath(id), [])
  return Array.isArray(list) ? list : []
}

/**
 * Save (or update) one session. Transient messages are dropped on the way in:
 * an error notice is worth showing once, not worth keeping forever.
 *
 * A session with nothing left to save is REMOVED rather than stored empty --
 * that is what makes "start a chat, send nothing, leave" not litter the drawer
 * with blank rows.
 */
export function save(id: string, msgs: ChatMessage[]): void {
  const keep = msgs.filter((m) => m.transient !== true)
  if (keep.length === 0) {
    remove(id)
    return
  }

  writeJson(idPath(id), keep)

  const next = index().filter((s) => s.id !== id)
  next.unshift({ id, title: titleOf(keep), updatedAt: Date.now(), count: keep.length })

  // Trim the tail, deleting each dropped session's transcript too -- an orphan
  // file would sit in the directory forever, invisible to the drawer.
  while (next.length > MAX_SESSIONS) {
    const dropped = next.pop()
    if (dropped !== undefined) {
      rmSync(idPath(dropped.id), { force: true })
    }
  }

  writeIndex(next)
}

export function remove(id: string): void {
  rmSync(idPath(id), { force: true })
  writeIndex(index().filter((s) => s.id !== id))
}

export function clear(): void {
  for (const s of index()) {
    rmSync(idPath(s.id), { force: true })
  }
  writeIndex([])
}

/**
 * Delete transcripts the index no longer lists.
 *
 * Nothing on the phone needs this -- preferences has no orphans, only keys --
 * but a directory of files does: a crash between writing a transcript and
 * writing the index leaves a file nothing will ever read or trim. Run at
 * startup, where it costs one readdir.
 */
export function sweepOrphans(): number {
  const known = new Set(index().map((s) => s.id + '.json'))
  known.add('index.json')

  let removed = 0
  for (const name of readdirSync(dir())) {
    if (!name.endsWith('.json') || known.has(name)) {
      continue
    }
    const path = join(dir(), name)
    if (existsSync(path)) {
      rmSync(path, { force: true })
      removed++
    }
  }
  if (removed > 0) {
    console.log(`[bodyclock] removed ${removed} orphaned transcript(s)`)
  }
  return removed
}

/** The first user message, clipped. Falls back to the first message of any role. */
function titleOf(msgs: ChatMessage[]): string {
  const first = msgs.find((m) => m.role === 'user' && m.content.length > 0) ?? msgs[0]
  return first === undefined ? '' : clip(first.content)
}

function clip(text: string): string {
  const oneLine = text.replace(/\s+/g, ' ').trim()
  return oneLine.length <= TITLE_LEN ? oneLine : oneLine.slice(0, TITLE_LEN) + '…'
}
