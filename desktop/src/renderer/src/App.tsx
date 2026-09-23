/**
 * The window's one screen: a thread, a composer, and the settings behind them.
 *
 * A port of harmony/entry/src/main/ets/pages/Index.ets, including the turn
 * lifecycle -- which is the part worth keeping identical, because it is where
 * the streamed pieces become one saved message and where "the model said
 * nothing" is told apart from "the user stopped it".
 */
import { useCallback, useEffect, useRef, useState, type JSX } from 'react'
import type { ChatMessage, MessagePart, Usage } from '@shared/chat'
import { ModelConfig, modelLabel } from '@shared/provider'
import {
  DEFAULT_PREFS,
  type CoreStatus,
  type InterfacePrefs,
  type SessionMeta
} from '@shared/store'
import { newSessionId, runTurn } from './chatEngine'
import * as omni from './omni'
import * as reader from './reader'
import * as tts from './tts'
import { Composer } from './components/Composer'
import { Settings } from './components/Settings'
import { Sidebar } from './components/Sidebar'
import { Thread } from './components/Thread'
import { S } from './strings'

type View = 'chat' | 'settings'

export function App(): JSX.Element {
  const [status, setStatus] = useState<CoreStatus | null>(null)
  const [prefs, setPrefs] = useState<InterfacePrefs>(DEFAULT_PREFS)
  const [systemDark, setSystemDark] = useState(
    () => window.matchMedia('(prefers-color-scheme: dark)').matches
  )

  const [models, setModels] = useState<ModelConfig[]>([])
  const [activeId, setActiveId] = useState('')

  const [sessions, setSessions] = useState<SessionMeta[]>([])
  const [sessionId, setSessionId] = useState(newSessionId)
  const [messages, setMessages] = useState<ChatMessage[]>([])

  const [input, setInput] = useState('')
  const [streaming, setStreaming] = useState(false)
  const [streamText, setStreamText] = useState('')
  const [streamThinking, setStreamThinking] = useState('')
  const [usage, setUsage] = useState<Usage | null>(null)
  const [view, setView] = useState<View>('chat')

  // The turn's accumulators and the transcript live in refs as well as state:
  // the callbacks below are created once per turn and would otherwise close
  // over the values as they were when the turn started, which for a stream of
  // deltas means every chunk overwriting the last.
  const textRef = useRef('')
  const thinkRef = useRef('')
  const messagesRef = useRef<ChatMessage[]>([])
  const cancelerRef = useRef<{ cancel: () => void } | null>(null)

  const setThread = useCallback((next: ChatMessage[]) => {
    messagesRef.current = next
    setMessages(next)
  }, [])

  //----------------------------------------------------------------------------
  // Startup
  //----------------------------------------------------------------------------

  const reloadModels = useCallback(async () => {
    setModels(await window.bodyclock.models.list())
    setActiveId(await window.bodyclock.models.activeId())
  }, [])

  const reloadSessions = useCallback(async () => {
    setSessions(await window.bodyclock.sessions.index())
  }, [])

  useEffect(() => {
    void (async () => {
      setStatus(await window.bodyclock.coreStatus())
      setPrefs(await window.bodyclock.prefs.get())
      await reloadModels()
      await reloadSessions()
    })()
  }, [reloadModels, reloadSessions])

  // 'system' is answered here and not in the stylesheet: an explicit choice has
  // to win over prefers-color-scheme, and a media query cannot be told that.
  useEffect(() => {
    const mq = window.matchMedia('(prefers-color-scheme: dark)')
    const onChange = (e: MediaQueryListEvent): void => setSystemDark(e.matches)
    mq.addEventListener('change', onChange)
    return () => mq.removeEventListener('change', onChange)
  }, [])

  const resolvedMode: 'light' | 'dark' =
    prefs.colorMode === 'system' ? (systemDark ? 'dark' : 'light') : prefs.colorMode

  useEffect(() => {
    const root = document.documentElement
    root.dataset.family = prefs.paletteFamily
    root.dataset.mode = resolvedMode
    root.style.setProperty('--bc-font-scale', String(prefs.fontScale))
  }, [prefs.paletteFamily, prefs.fontScale, resolvedMode])

  const changePrefs = (patch: Partial<InterfacePrefs>): void => {
    void window.bodyclock.prefs.set(patch).then(setPrefs)
  }

  //----------------------------------------------------------------------------
  // Sessions
  //----------------------------------------------------------------------------

  const persist = useCallback(
    async (id: string, msgs: ChatMessage[]) => {
      await window.bodyclock.sessions.save(id, msgs)
      await reloadSessions()
    },
    [reloadSessions]
  )

  const openSession = async (id: string): Promise<void> => {
    if (streaming) {
      return // switching mid-turn would stream into the wrong transcript
    }
    setSessionId(id)
    setThread(await window.bodyclock.sessions.messages(id))
    setUsage(null)
    setView('chat')
  }

  const newSession = (): void => {
    if (streaming) {
      return
    }
    // The engines are told as well as the screen. Clearing the transcript is
    // enough for a remote provider, which is handed the whole conversation every
    // turn and keeps nothing; an on-device one keeps a KV cache, and the next
    // prompt having one message in it does not by itself empty it.
    void window.bodyclock.chat.forget().catch(() => undefined)
    setSessionId(newSessionId())
    setThread([])
    setUsage(null)
    setView('chat')
  }

  const dropSession = async (id: string): Promise<void> => {
    await window.bodyclock.sessions.remove(id)
    await reloadSessions()
    if (id === sessionId) {
      newSession()
    }
  }

  const clearSessions = async (): Promise<void> => {
    await window.bodyclock.sessions.clear()
    await reloadSessions()
    newSession()
  }

  //----------------------------------------------------------------------------
  // One turn
  //----------------------------------------------------------------------------

  /**
   * Say something to the user in the thread. Transient rows render like a reply
   * but are never sent to a model and never saved.
   */
  const note = (text: string): void => {
    setThread([
      ...messagesRef.current,
      { role: 'assistant', content: text, transient: true, ts: Date.now() }
    ])
  }

  /**
   * Fold the streamed pieces into one assistant message and persist.
   *
   * A turn that produced nothing at all still gets a row: silence after a send
   * reads as the app being broken, and "the model returned nothing" is at least
   * actionable. An aborted turn keeps whatever arrived -- the user stopped it,
   * they did not ask to lose it.
   */
  const finishTurn = (label: string, aborted: boolean, id: string): void => {
    const parts: MessagePart[] = []
    if (thinkRef.current.length > 0) {
      parts.push({ kind: 'thinking', content: thinkRef.current })
    }
    if (textRef.current.length > 0) {
      parts.push({ kind: 'text', content: textRef.current })
    }

    let next = messagesRef.current
    if (parts.length > 0) {
      next = [
        ...next,
        {
          role: 'assistant',
          content: textRef.current,
          parts,
          model: label,
          ts: Date.now()
        }
      ]
    } else if (!aborted) {
      const last = next[next.length - 1]
      if (last === undefined || last.transient !== true) {
        next = [
          ...next,
          {
            role: 'assistant',
            content: S.chatEmptyReply,
            transient: true,
            ts: Date.now()
          }
        ]
      }
    }
    setThread(next)

    setStreaming(false)
    setStreamText('')
    setStreamThinking('')
    textRef.current = ''
    thinkRef.current = ''
    cancelerRef.current = null
    void persist(id, next)
  }

  const send = (): void => {
    const text = input.trim()
    if (text.length === 0 || streaming) {
      return
    }
    if (activeId.length === 0) {
      note(S.chatNeedModel)
      return
    }

    const withUser: ChatMessage[] = [
      ...messagesRef.current,
      { role: 'user', content: text, ts: Date.now() }
    ]
    setThread(withUser)
    setInput('')
    setStreaming(true)
    setStreamText('')
    setStreamThinking('')
    setUsage(null)
    textRef.current = ''
    thinkRef.current = ''

    const active = models.find((m) => m.id === activeId)
    const label = active === undefined ? activeId : modelLabel(active)
    const turnSession = sessionId

    // A new reply supersedes the old one out loud as well as on screen.
    // Unconditional: stopping when nothing is speaking costs an atomic flag.
    void tts.stop()

    // The system prompt is read HERE, not held in state, for the reason the
    // speak flag taught: a value captured at startup is wrong the moment the
    // user changes it, and the settings screen is where they change it. One
    // extra round trip against a reply that takes seconds.
    //
    // Everything the user can see has already been updated above, so the await
    // delays the request and not the interface.
    void (async () => {
      // Omni answers INSTEAD of the chain when it is switched on: one model
      // that hears, thinks and speaks, so there is no turn to stream and no
      // separate speech step.
      //
      // There is still something to show early, and it is not a token stream.
      // The thinker writes the whole reply and stops -- measured, 3.8 s -- and
      // the Talker then spends most of a minute saying it. onText puts those
      // words on screen at the moment they exist, so the page is not silent
      // while the speech it is reading out plays.
      const om = await window.bodyclock.omni.status()
      if (om.available && om.loaded && om.enabled) {
        try {
          const r = await omni.ask(text, (t) => {
            textRef.current = t
            setStreamText(t)
          })
          setThread([
            ...messagesRef.current,
            {
              role: 'assistant',
              content: r.text,
              model: 'Qwen2.5-Omni',
              ts: Date.now()
            }
          ])
          void persist(turnSession, messagesRef.current)
        } catch (err) {
          note(err instanceof Error ? err.message : String(err))
          // A failure AFTER the thinker is a failure of the voice, not of the
          // answer. The words are on screen by then and throwing them away
          // would be the worse half of the reply to lose.
          if (textRef.current.length > 0) {
            setThread([
              ...messagesRef.current,
              {
                role: 'assistant',
                content: textRef.current,
                model: 'Qwen2.5-Omni',
                ts: Date.now()
              }
            ])
            void persist(turnSession, messagesRef.current)
          }
        }
        // The reply has moved from the streaming bubble into the thread, so the
        // bubble has to be emptied -- it used to be empty for this whole lane
        // and is not any more.
        setStreaming(false)
        setStreamText('')
        textRef.current = ''
        cancelerRef.current = null
        return
      }

      const { systemPrompt } = await window.bodyclock.chat.settings()

      // The reading lane, decided BEFORE the turn starts because it has to be
      // open before the first token arrives. Asked fresh for the same reason
      // the speak flag is: the models load in the background.
      const reading = await reader.ready()
      if (reading) {
        await reader.begin()
      }

      cancelerRef.current = runTurn(activeId, withUser, systemPrompt, {
        onDelta: (t) => {
          textRef.current += t
          setStreamText(textRef.current)
          // The same delta the transcript gets. Not awaited: this runs once per
          // token, and an IPC round trip per token would put the page's own
          // rendering behind the main process.
          if (reading) {
            reader.feed(t)
          }
        },
        onThinking: (t) => {
          thinkRef.current += t
          setStreamThinking(thinkRef.current)
        },
        onUsage: (json) => {
          try {
            setUsage(JSON.parse(json) as Usage)
          } catch {
            // A summary that will not parse is worth nothing and costs nothing to
            // drop; the reply itself already arrived.
          }
        },
        onError: (message) => {
          setThread([
            ...messagesRef.current,
            {
              role: 'assistant',
              content: message,
              transient: true,
              ts: Date.now()
            }
          ])
        },
        onDone: (aborted) => {
          // ONE utterance for the whole reply, once it is whole.
          //
          // Not sentence by sentence as it streamed: each say() is an independent
          // generation with its own prosody arc, so consecutive ones are audibly
          // stitched however they are cut. Waiting costs almost nothing here --
          // measured RTF 0.56 with first audio 0.13 s -- so the speech starts
          // immediately after the reply does, and never breaks.
          //
          // Asked NOW rather than remembered from startup. The models load in the
          // background, so at the moment the window opened `loaded` was still
          // false; a flag captured then said "do not speak" for the rest of the
          // session, and only opening the settings screen refreshed it -- which
          // is why the first reply was silent until the test button had been
          // pressed.
          //
          // Skipped when the user stopped the turn: they asked for silence.
          const reply = textRef.current
          if (reading) {
            // The stream has to be closed either way -- there are engines and
            // threads behind it -- but on an abort the audio is dropped rather
            // than played, which the player's stop() below does.
            void reader.finish().catch(() => undefined)
            if (aborted) {
              void tts.stop()
            }
          } else if (!aborted && reply.length > 0) {
            void (async () => {
              const st = await window.bodyclock.tts.status()
              if (st.available && st.loaded && st.autoSpeak) {
                void tts.speak(turnSession, reply)
              }
            })()
          }
          finishTurn(label, aborted, turnSession)
        }
      })
    })()
  }

  const stop = (): void => cancelerRef.current?.cancel()

  //----------------------------------------------------------------------------
  // Render
  //----------------------------------------------------------------------------

  const ready = activeId.length > 0 && status?.ok === true

  return (
    <div className="shell">
      <Sidebar
        sessions={sessions}
        currentId={sessionId}
        onSelect={(id) => void openSession(id)}
        onNew={newSession}
        onDelete={(id) => void dropSession(id)}
        onOpenSettings={() => setView('settings')}
      />

      <main className="main">
        {view === 'settings' && status !== null ? (
          <Settings
            status={status}
            models={models}
            prefs={prefs}
            resolvedMode={resolvedMode}
            onModelsChanged={reloadModels}
            onPrefs={changePrefs}
            onClearSessions={() => void clearSessions()}
            onClose={() => setView('chat')}
          />
        ) : (
          <>
            <div className="topbar">
              <select
                className="control"
                value={activeId}
                disabled={models.length === 0}
                onChange={(e) => {
                  setActiveId(e.target.value)
                  void window.bodyclock.models.setActiveId(e.target.value)
                }}
              >
                {models.length === 0 ? (
                  <option value="">{S.chatPickModel}</option>
                ) : (
                  models.map((m) => (
                    <option key={m.id} value={m.id}>
                      {modelLabel(m)}
                    </option>
                  ))
                )}
              </select>
              <div className="topbar-spacer" />
              <button className="btn btn-quiet" onClick={newSession} disabled={streaming}>
                {S.drawerNewChat}
              </button>
            </div>

            {status !== null && !status.ok && <div className="banner">{status.detail}</div>}

            {messages.length === 0 && !streaming ? (
              <div className="thread">
                <div className="placeholder">
                  <h2>{S.chatEmptyTitle}</h2>
                  <p>{S.chatEmptyBody}</p>
                </div>
              </div>
            ) : (
              <Thread
                messages={messages}
                streaming={streaming}
                streamText={streamText}
                streamThinking={streamThinking}
                usage={usage}
              />
            )}

            <Composer
              value={input}
              onChange={setInput}
              onSend={send}
              onStop={stop}
              streaming={streaming}
              ready={ready}
              hint={S.chatDisclaimer}
            />
          </>
        )}
      </main>
    </div>
  )
}
