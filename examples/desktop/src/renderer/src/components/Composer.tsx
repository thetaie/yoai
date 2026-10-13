/** The input. Enter sends, Shift+Enter breaks a line. */
import { useEffect, useRef, type JSX, type KeyboardEvent } from 'react'

interface Props {
  value: string
  onChange: (text: string) => void
  onSend: () => void
  onStop: () => void
  streaming: boolean
  /**
   * Whether there is speech to stop. It outlasts the turn -- the engine writes
   * faster than the speaker plays -- so 停止 stays beside 发送 until it ends.
   */
  speaking: boolean
  /** False when no model is configured; the box still takes text and says why. */
  ready: boolean
  hint: string
}

export function Composer({
  value,
  onChange,
  onSend,
  onStop,
  streaming,
  speaking,
  ready,
  hint
}: Props): JSX.Element {
  const box = useRef<HTMLTextAreaElement>(null)

  // Grow with the text. Reset to auto first, or the box only ever gets taller:
  // scrollHeight cannot report less than the height already set on the element.
  useEffect(() => {
    const el = box.current
    if (el !== null) {
      el.style.height = 'auto'
      el.style.height = `${el.scrollHeight}px`
    }
  }, [value])

  const onKeyDown = (e: KeyboardEvent<HTMLTextAreaElement>): void => {
    // An IME composing a Chinese word ends a candidate with Enter. Sending on
    // that would cut the sentence off mid-word, so the composition state is
    // checked before the key is read as "send".
    if (e.key === 'Enter' && !e.shiftKey && !e.nativeEvent.isComposing) {
      e.preventDefault()
      onSend()
    }
  }

  return (
    <div className="composer">
      <div className="composer-inner">
        <textarea
          ref={box}
          rows={1}
          value={value}
          placeholder={ready ? '说点什么' : '先在设置里添加一个模型'}
          onChange={(e) => onChange(e.target.value)}
          onKeyDown={onKeyDown}
        />
        {(streaming || speaking) && (
          <button className="btn" onClick={onStop}>
            停止
          </button>
        )}
        {!streaming && (
          <button
            className="btn btn-primary"
            onClick={onSend}
            disabled={!ready || value.trim().length === 0}
          >
            发送
          </button>
        )}
      </div>
      {hint.length > 0 && <p className="composer-hint">{hint}</p>}
    </div>
  )
}
