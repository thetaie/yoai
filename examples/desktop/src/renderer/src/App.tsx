/**
 * The window's one screen: a thread, a composer, and the settings behind them.
 *
 * A port of examples/harmony/entry/src/main/ets/pages/Index.ets, including the turn
 * lifecycle -- which is the part worth keeping identical, because it is where
 * the streamed pieces become one saved message and where "the model said
 * nothing" is told apart from "the user stopped it".
 */
import { useCallback, useEffect, useRef, useState, useSyncExternalStore, type JSX } from 'react'
import type { ChatMessage, MessagePart, Usage } from '@shared/chat'
import { ModelConfig, modelLabel } from '@shared/provider'
import {
  DEFAULT_PREFS,
  type CoreStatus,
  type InterfacePrefs,
  type SessionMeta
} from '@shared/store'
import type { AsrStatus } from '@shared/api'
import { player } from './audio'
import { newSessionId, runTurn } from './chatEngine'
import { VoiceController, type VoiceHost } from './voice'
import * as omni from './omni'
import * as reader from './reader'
import * as tts from './tts'
import { Composer } from './components/Composer'
import { Settings } from './components/Settings'
import { Sidebar } from './components/Sidebar'
import { Thread } from './components/Thread'
import { S } from './strings'

type View = 'chat' | 'settings'

/** How a turn ended: answered, stopped, or -- a live turn -- the model waited. */
type TurnEnd = 'done' | 'stopped' | 'wait'

interface TurnOptions {
  voice?: boolean
  live?: boolean
  join?: boolean
}

/**
 * The whole reply a live turn's model gives when the user's speech has nothing
 * to answer yet: YOAI_WAIT_MARKER in src/yoai.h, which the core's prompt asks
 * for. Never shown, read or kept.
 */
const WAIT_MARKER = '<wait>'

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
  // Whether there is speech to stop, from any lane, for as long as the speaker
  // has any of it -- which outlasts the turn that produced it.
  const speaking = useSyncExternalStore(player.watch, player.active)

  // The turn's accumulators and the transcript live in refs as well as state:
  // the callbacks below are created once per turn and would otherwise close
  // over the values as they were when the turn started, which for a stream of
  // deltas means every chunk overwriting the last.
  const textRef = useRef('')
  const thinkRef = useRef('')
  const messagesRef = useRef<ChatMessage[]>([])
  const cancelerRef = useRef<{ cancel: () => void } | null>(null)
  // `streaming`, for the voice controller, which runs between renders.
  const streamingRef = useRef(false)
  // Who is waiting for the turn to end; see startTurn().
  const turnEndRef = useRef<((how: TurnEnd) => void) | null>(null)
  // A live turn's model answered WAIT_MARKER.
  const waitedRef = useRef(false)
  // A stopped reply keeps this much of its text: what was heard of it. null
  // keeps all of it, as a stop from the 停止 button does.
  const keepRef = useRef<string | null>(null)

  const setThread = useCallback((next: ChatMessage[]) => {
    messagesRef.current = next
    setMessages(next)
  }, [])

  const markStreaming = useCallback((on: boolean) => {
    streamingRef.current = on
    setStreaming(on)
  }, [])

  //----------------------------------------------------------------------------
  // Startup
  //----------------------------------------------------------------------------

  const reloadModels = useCallback(async () => {
    setModels(await window.yoai.models.list())
    setActiveId(await window.yoai.models.activeId())
  }, [])

  const reloadSessions = useCallback(async () => {
    setSessions(await window.yoai.sessions.index())
  }, [])

  useEffect(() => {
    void (async () => {
      setStatus(await window.yoai.coreStatus())
      setPrefs(await window.yoai.prefs.get())
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
    void window.yoai.prefs.set(patch).then(setPrefs)
  }

  //----------------------------------------------------------------------------
  // Sessions
  //----------------------------------------------------------------------------

  const persist = useCallback(
    async (id: string, msgs: ChatMessage[]) => {
      await window.yoai.sessions.save(id, msgs)
      await reloadSessions()
    },
    [reloadSessions]
  )

  const openSession = async (id: string): Promise<void> => {
    if (streaming) {
      return // switching mid-turn would stream into the wrong transcript
    }
    // A voice conversation belongs to the transcript it was held in.
    voiceRef.current?.close()
    setSessionId(id)
    setThread(await window.yoai.sessions.messages(id))
    setUsage(null)
    setView('chat')
  }

  const newSession = (): void => {
    if (streaming) {
      return
    }
    voiceRef.current?.close()
    // The engines are told as well as the screen. Clearing the transcript is
    // enough for a remote provider, which is handed the whole conversation every
    // turn and keeps nothing; an on-device one keeps a KV cache, and the next
    // prompt having one message in it does not by itself empty it.
    void window.yoai.chat.forget().catch(() => undefined)
    setSessionId(newSessionId())
    setThread([])
    setUsage(null)
    setView('chat')
  }

  const dropSession = async (id: string): Promise<void> => {
    await window.yoai.sessions.remove(id)
    await reloadSessions()
    if (id === sessionId) {
      newSession()
    }
  }

  const clearSessions = async (): Promise<void> => {
    await window.yoai.sessions.clear()
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
    // A live turn the model answered <wait> leaves nothing; one stopped by a
    // voice keeps what was heard of it (keepRef), or nothing.
    const waited = waitedRef.current
    const keep = aborted ? keepRef.current : null
    waitedRef.current = false
    keepRef.current = null
    let text = textRef.current
    let thinking = thinkRef.current
    if (waited || keep === '') {
      text = ''
      thinking = ''
    } else if (keep !== null) {
      text = keep
    }

    const parts: MessagePart[] = []
    if (thinking.length > 0) {
      parts.push({ kind: 'thinking', content: thinking })
    }
    if (text.length > 0) {
      parts.push({ kind: 'text', content: text })
    }

    let next = messagesRef.current
    if (parts.length > 0) {
      next = [
        ...next,
        {
          role: 'assistant',
          content: text,
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

    markStreaming(false)
    setStreamText('')
    setStreamThinking('')
    textRef.current = ''
    thinkRef.current = ''
    cancelerRef.current = null
    void persist(id, next)

    const end = turnEndRef.current
    turnEndRef.current = null
    end?.(waited ? 'wait' : aborted ? 'stopped' : 'done')
  }

  /** What the box holds, sent. */
  const send = (): void => {
    const text = input.trim()
    if (text.length === 0 || streaming) {
      return
    }
    if (activeId.length === 0) {
      note(S.chatNeedModel)
      return
    }
    setInput('')
    void startTurn(text)
  }

  /**
   * Send `text` as the user and stream the reply. Resolves when the turn is
   * over, saying how: 'wait' when a live turn's model answered WAIT_MARKER.
   *
   * `voice`: the text was spoken, so the reply is read aloud whenever the voice
   * is loaded, whatever the reading setting says. `live`: a full-duplex turn,
   * which the model may answer <wait> alone. `join`: the text replaces the last
   * user message instead of following it -- the rest of a request held for it.
   */
  const startTurn = (text: string, opts: TurnOptions = {}): Promise<TurnEnd> => {
    if (text.length === 0 || streamingRef.current) {
      return Promise.resolve('stopped')
    }
    if (activeId.length === 0) {
      note(S.chatNeedModel)
      return Promise.resolve('stopped')
    }

    const prior = messagesRef.current
    const base =
      opts.join === true && prior[prior.length - 1]?.role === 'user' ? prior.slice(0, -1) : prior
    const withUser: ChatMessage[] = [...base, { role: 'user', content: text, ts: Date.now() }]
    setThread(withUser)
    markStreaming(true)
    setStreamText('')
    setStreamThinking('')
    setUsage(null)
    textRef.current = ''
    thinkRef.current = ''
    waitedRef.current = false
    const ended = new Promise<TurnEnd>((resolve) => {
      turnEndRef.current = resolve
    })

    const active = models.find((m) => m.id === activeId)
    const label = active === undefined ? activeId : modelLabel(active)
    const turnSession = sessionId

    // A new reply supersedes the old one out loud as well as on screen.
    // Unconditional: stopping when nothing is speaking costs an atomic flag.
    void tts.stop()

    // The system prompt is not read here: the main process adds it when the
    // turn starts, from the settings as they stand then, so a change on the
    // settings screen applies to the very next message.
    //
    // Everything the user can see has already been updated above, so the awaits
    // below delay the request and not the interface.
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
      const om = await window.yoai.omni.status()
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
        markStreaming(false)
        setStreamText('')
        textRef.current = ''
        cancelerRef.current = null
        const end = turnEndRef.current
        turnEndRef.current = null
        end?.('done')
        return
      }

      // The reading lane, decided BEFORE the turn starts because it has to be
      // open before the first token arrives. Asked fresh for the same reason
      // the speak flag is: the models load in the background. A spoken
      // message is answered aloud whenever there is a voice to do it.
      const reading = opts.voice === true ? await reader.loaded() : await reader.ready()
      if (reading) {
        await reader.begin()
      }

      // A live turn's reply is held back until it cannot be WAIT_MARKER any
      // more: a token or two. The marker is never shown, read or kept.
      let head = ''
      let decided = opts.live !== true

      // A reply that will be heard is asked to be written for the ear. The
      // main process builds that prompt the way the local model's warm-up
      // does, so the prompt computed ahead of this turn is the one it sends.
      cancelerRef.current = runTurn(activeId, withUser, reading, {
        onDelta: (delta) => {
          let t = delta
          if (!decided) {
            head += t
            const lead = head.trimStart()
            if (lead.length < WAIT_MARKER.length && WAIT_MARKER.startsWith(lead)) {
              return
            }
            decided = true
            if (lead.startsWith(WAIT_MARKER)) {
              waitedRef.current = true
              cancelerRef.current?.cancel()
              return
            }
            t = head
          }
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
          // A reply shorter than the marker that began like it: it was a reply.
          if (!decided && head.length > 0) {
            textRef.current += head
            if (reading) {
              reader.feed(head)
            }
          }
          if (reading) {
            // The stream has to be closed either way -- there are engines and
            // threads behind it. When the user stopped the turn they asked for
            // silence, so it is cancelled rather than read to the end.
            if (aborted) {
              void reader.cancel()
            } else {
              void reader.finish().catch(() => undefined)
            }
          }
          finishTurn(label, aborted, turnSession)
        }
      }, opts.live === true)
    })()
    return ended
  }

  /**
   * Silence, from whichever lane is speaking and whatever state its engine is
   * in: the reading lane (a reply, a message's 🔊, the settings test), omni,
   * and anything else on the player, such as a recorded take.
   */
  const stopSpeech = (): void => {
    void tts.stop()
    omni.cancel()
    player.stop()
  }

  /** The composer's 停止: the turn if one is running, and the speech. */
  const stop = (): void => {
    cancelerRef.current?.cancel()
    stopSpeech()
  }

  //----------------------------------------------------------------------------
  // Voice
  //----------------------------------------------------------------------------

  /** The last reply in the thread, which may still be being heard. */
  const lastReplyIndex = (): number => {
    const msgs = messagesRef.current
    for (let i = msgs.length - 1; i >= 0; i--) {
      if (msgs[i].role === 'assistant' && msgs[i].transient !== true) {
        return i
      }
      if (msgs[i].role === 'user') {
        return -1
      }
    }
    return -1
  }

  /**
   * Stop the reply for a voice that took the turn back. `heard` is what the
   * reply keeps -- '' for none of it -- or null for all of it. A reply still
   * being written is kept when its turn ends (finishTurn); one already in the
   * thread and still being read is cut there and then.
   */
  const stopReply = (heard: string | null): Promise<void> => {
    if (streamingRef.current) {
      keepRef.current = heard
      const done = new Promise<void>((resolve) => {
        const waiting = turnEndRef.current
        turnEndRef.current = (how) => {
          waiting?.(how)
          resolve()
        }
      })
      stop()
      return done
    }
    stopSpeech()
    const i = lastReplyIndex()
    if (heard !== null && i >= 0) {
      const msgs = messagesRef.current
      const next =
        heard.length === 0
          ? [...msgs.slice(0, i), ...msgs.slice(i + 1)]
          : msgs.map((m, k) =>
              k === i ? { ...m, content: heard, parts: [{ kind: 'text' as const, content: heard }] } : m
            )
      setThread(next)
      void persist(sessionId, next)
    }
    return Promise.resolve()
  }

  const voiceHost: VoiceHost = {
    writing: () => streamingRef.current,
    replyText: () => {
      if (streamingRef.current) {
        return textRef.current
      }
      const i = lastReplyIndex()
      return player.active() && i >= 0 ? messagesRef.current[i].content : ''
    },
    lastUserText: () => {
      const msgs = messagesRef.current
      for (let i = msgs.length - 1; i >= 0; i--) {
        if (msgs[i].role === 'user') {
          return msgs[i].content
        }
      }
      return ''
    },
    send: (text, live, join) => startTurn(text, { voice: true, live, join }),
    show: (text, join) => {
      const prior = messagesRef.current
      const base = join && prior[prior.length - 1]?.role === 'user' ? prior.slice(0, -1) : prior
      const next: ChatMessage[] = [...base, { role: 'user', content: text, ts: Date.now() }]
      setThread(next)
      void persist(sessionId, next)
    },
    stopReply,
    note
  }
  const voiceRef = useRef<VoiceController | null>(null)
  if (voiceRef.current === null) {
    voiceRef.current = new VoiceController(voiceHost)
  } else {
    voiceRef.current.setHost(voiceHost)
  }
  const voice = voiceRef.current
  const micState = useSyncExternalStore(voice.watch, voice.current)

  // Whether the microphone can be offered at all: recognition built in. Whether
  // its model is loaded is asked on each click (VoiceController.click).
  const [asrStatus, setAsrStatus] = useState<AsrStatus | null>(null)
  useEffect(() => {
    if (view === 'chat') {
      void window.yoai.asr.status().then(setAsrStatus)
    }
  }, [view])

  // The microphone goes with the window.
  useEffect(() => () => voice.close(), [voice])

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
        streaming={streaming}
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
            speaking={speaking}
            onStopSpeech={stopSpeech}
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
                  void window.yoai.models.setActiveId(e.target.value)
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
              speaking={speaking}
              mic={asrStatus?.available === true ? micState : null}
              onMic={() => void voice.click()}
              ready={ready}
              hint={S.chatDisclaimer}
            />
          </>
        )}
      </main>
    </div>
  )
}
