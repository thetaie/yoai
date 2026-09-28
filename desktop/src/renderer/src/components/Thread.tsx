/**
 * The conversation, plus whatever the current turn has produced so far.
 *
 * A reply arrives in two streams -- 'thinking' and 'reply' -- and they are kept
 * apart all the way to the screen, the way src/yoai.h delivers them: the
 * reasoning is collapsed, because a reasoning model's chain-of-thought is not
 * part of the answer.
 */
import { useEffect, useRef, useState, useSyncExternalStore, type JSX } from 'react'
import type { ChatMessage, Usage } from '@shared/chat'
import { S } from '../strings'
import * as tts from '../tts'
import { Markdown } from './Markdown'

interface Props {
  messages: ChatMessage[]
  streaming: boolean
  streamText: string
  streamThinking: string
  /** The last finished turn's token summary, or null. */
  usage: Usage | null
}

/**
 * Which message is being spoken, as a React subscription.
 *
 * useSyncExternalStore rather than a useState + effect pair because the store
 * is genuinely external -- one module singleton fronting one engine -- and this
 * is the hook that exists for exactly that, without the render-then-catch-up
 * flicker an effect would add.
 */
function useSpeaking(): string {
  return useSyncExternalStore(tts.watch, tts.speakingOf, tts.speakingOf)
}

/** "1,024 tokens in · 512 out · $0.0031" -- absent fields are simply left out. */
function usageLine(u: Usage): string {
  const bits: string[] = []
  if (u.inputTokens !== undefined) bits.push(`${u.inputTokens.toLocaleString()} tokens in`)
  if (u.outputTokens !== undefined) bits.push(`${u.outputTokens.toLocaleString()} out`)
  if (u.thoughtTokens !== undefined && u.thoughtTokens > 0) {
    bits.push(`${u.thoughtTokens.toLocaleString()} thinking`)
  }
  // Only a real cost. An on-device turn reports 0, and saying so on every reply
  // is a line of text to tell the reader nothing happened -- the absence of a
  // price is already the message.
  if (u.totalCost !== undefined && u.totalCost > 0) {
    bits.push(`$${u.totalCost.toFixed(4)}`)
  }
  return bits.join(' · ')
}

function MessageView({
  m,
  id,
  ttsReady,
  usage
}: {
  m: ChatMessage
  id: string
  ttsReady: boolean
  /** The finished turn's token summary, on the message it belongs to. */
  usage: Usage | null
}): JSX.Element {
  const parts = m.parts ?? [{ kind: 'text' as const, content: m.content }]

  // The thinking is deliberately not spoken: it is not part of the answer, and
  // reading a model's chain-of-thought aloud is minutes of text nobody asked
  // for. Same rule that keeps it collapsed on screen.
  const spoken = parts
    .filter((p) => p.kind !== 'thinking')
    .map((p) => p.content)
    .join('\n')

  const speaking = useSpeaking() === id
  const offer = ttsReady && m.role === 'assistant' && spoken.trim().length > 0

  return (
    <div className="msg" data-role={m.role} data-transient={m.transient === true}>
      {parts.map((p, i) =>
        p.kind === 'thinking' ? (
          <details className="thinking" key={i}>
            <summary>思考过程</summary>
            <div className="thinking-body">{p.content}</div>
          </details>
        ) : (
          // A reply is markdown; what the user typed is shown as typed. A
          // transient message is the app's own -- an error -- and plain too.
          <div className="bubble" key={i}>
            {m.role === 'assistant' && m.transient !== true ? (
              <Markdown text={p.content} />
            ) : (
              p.content
            )}
          </div>
        )
      )}
      {/* Rendered only when it has something in it. An always-present row is an
          empty flex line under every user message, costing its own line-height
          and the gap above it for nothing. */}
      {(m.model !== undefined || offer || usage !== null) && (
        <div className="msg-meta">
          {m.model !== undefined && <span>{m.model}</span>}
          {usage !== null && <span className="msg-usage">{usageLine(usage)}</span>}
          {offer && (
            <button
              className="link"
              onClick={() => void (speaking ? tts.stop() : tts.speak(id, spoken))}
            >
              {speaking ? S.ttsStop : S.ttsSpeak}
            </button>
          )}
        </div>
      )}
    </div>
  )
}

export function Thread({
  messages,
  streaming,
  streamText,
  streamThinking,
  usage
}: Props): JSX.Element {
  const scroller = useRef<HTMLDivElement>(null)

  // Asked once: whether this build can speak at all and has its models loaded.
  // The buttons are simply absent otherwise, rather than present and refusing.
  // The reading lane, which is the one that speaks; see tts.ts.
  const [ttsReady, setTtsReady] = useState(false)
  useEffect(() => {
    void (async () => {
      const st = await window.yoai.reader.status()
      setTtsReady(st.available && st.loaded)
    })()
  }, [messages.length])

  // Follow the reply only while the user is already at the bottom. Tracked on
  // the SCROLL event rather than measured at render time: by the time an effect
  // runs the new text is in the DOM, so the distance it would read is the one
  // after the content grew, which says "not at the bottom" for every long reply
  // and leaves the thread stuck halfway up.
  const stick = useRef(true)
  const onScroll = (): void => {
    const el = scroller.current
    if (el !== null) {
      stick.current = el.scrollHeight - el.scrollTop - el.clientHeight < 80
    }
  }

  useEffect(() => {
    const el = scroller.current
    if (el !== null && stick.current) {
      el.scrollTop = el.scrollHeight
    }
  }, [messages, streamText, streamThinking])

  return (
    <div className="thread" ref={scroller} onScroll={onScroll}>
      <div className="thread-inner">
        {messages.map((m, i) => (
          <MessageView
            key={i}
            id={`m${i}`}
            m={m}
            ttsReady={ttsReady}
            // On the LAST assistant message rather than under the thread: the
            // summary describes the turn that message is, and as a sibling of
            // the messages it inherited the gap meant for the space BETWEEN
            // them, so it floated halfway and belonged to neither.
            usage={!streaming && i === messages.length - 1 && m.role === 'assistant' ? usage : null}
          />
        ))}

        {streaming && (
          <div className="msg" data-role="assistant">
            {streamThinking.length > 0 && (
              <details className="thinking" open>
                <summary>思考中</summary>
                <div className="thinking-body">{streamThinking}</div>
              </details>
            )}
            <div className="bubble">
              <Markdown text={streamText} />
              <span className="caret" />
            </div>
          </div>
        )}
      </div>
    </div>
  )
}
