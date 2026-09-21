/**
 * The conversation, plus whatever the current turn has produced so far.
 *
 * A reply arrives in two streams -- 'thinking' and 'reply' -- and they are kept
 * apart all the way to the screen, the way src/bodyclock.h delivers them: the
 * reasoning is collapsed, because a reasoning model's chain-of-thought is not
 * part of the answer.
 */
import { useEffect, useRef, type JSX } from 'react'
import type { ChatMessage, Usage } from '@shared/chat'

interface Props {
  messages: ChatMessage[]
  streaming: boolean
  streamText: string
  streamThinking: string
  /** The last finished turn's token summary, or null. */
  usage: Usage | null
}

/** "1,024 tokens in · 512 out · $0.0031" -- absent fields are simply left out. */
function usageLine(u: Usage): string {
  const bits: string[] = []
  if (u.inputTokens !== undefined) bits.push(`${u.inputTokens.toLocaleString()} tokens in`)
  if (u.outputTokens !== undefined) bits.push(`${u.outputTokens.toLocaleString()} out`)
  if (u.thoughtTokens !== undefined && u.thoughtTokens > 0) {
    bits.push(`${u.thoughtTokens.toLocaleString()} thinking`)
  }
  // An on-device turn reports a cost of 0 and that is worth showing as free
  // rather than hiding -- the token figures are real either way.
  if (u.totalCost !== undefined) {
    bits.push(u.totalCost > 0 ? `$${u.totalCost.toFixed(4)}` : '本地，无费用')
  }
  return bits.join(' · ')
}

function MessageView({ m }: { m: ChatMessage }): JSX.Element {
  const parts = m.parts ?? [{ kind: 'text' as const, content: m.content }]
  return (
    <div className="msg" data-role={m.role} data-transient={m.transient === true}>
      {parts.map((p, i) =>
        p.kind === 'thinking' ? (
          <details className="thinking" key={i}>
            <summary>思考过程</summary>
            <div className="thinking-body">{p.content}</div>
          </details>
        ) : (
          <div className="bubble" key={i}>
            {p.content}
          </div>
        )
      )}
      {m.model !== undefined && <div className="msg-meta">{m.model}</div>}
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
          <MessageView key={i} m={m} />
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
              {streamText}
              <span className="caret" />
            </div>
          </div>
        )}

        {!streaming && usage !== null && <div className="usage">{usageLine(usage)}</div>}
      </div>
    </div>
  )
}
