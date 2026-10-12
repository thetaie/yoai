/**
 * Recording a reference voice: read a passage aloud, listen back, use it.
 *
 * A recording anchors better than a synthesized reference -- one real speaker
 * held the pieces of a reply to 1.31 semitones on average, a clone of one to
 * 1.68 (src/voice/tts/reader.hpp) -- so this is the first way offered to make a
 * voice of one's own. The passage is plain, even prose, because what the
 * embedding captures is how the voice sounds, not what it says.
 *
 * The take is kept as a WAV under userData and then used exactly as a picked
 * .wav is: extracted once for the model size, cached, and applied at once.
 */
import { useEffect, useRef, useState, type JSX } from 'react'
import { player } from '../audio'
import { record, REFERENCE_RATE, toWav, type Take } from '../recorder'
import { S } from '../strings'

/**
 * Below this a take is not offered for use.
 *
 * The references that were measured were 17 and 22 seconds long; nothing
 * shorter has been tried, so this is a floor against an accidental click
 * rather than a measured limit.
 */
const MIN_SECONDS = 10

/** At or above this the take has hit full scale somewhere, and is clipped. */
const CLIP = 0.99

type Phase = 'idle' | 'recording' | 'recorded' | 'saving'

export function VoiceRecorder({
  onUsed,
  onClose
}: {
  /** The take is now the reference voice. */
  onUsed: () => void
  onClose: () => void
}): JSX.Element {
  const [phase, setPhase] = useState<Phase>('idle')
  const [seconds, setSeconds] = useState(0)
  const [level, setLevel] = useState(0)
  const [clipped, setClipped] = useState(false)
  const [error, setError] = useState('')
  const take = useRef<Take | null>(null)
  const pcm = useRef<Float32Array | null>(null)

  // The meter, while recording.
  useEffect(() => {
    if (phase !== 'recording') {
      return
    }
    const timer = setInterval(() => {
      const t = take.current
      if (t !== null) {
        setSeconds(t.seconds())
        setLevel(t.level())
        setClipped(t.peak() >= CLIP)
      }
    }, 100)
    return () => clearInterval(timer)
  }, [phase])

  // Leaving the page mid-take lets go of the microphone.
  useEffect(
    () => () => {
      take.current?.cancel()
      player.stop()
    },
    []
  )

  const start = async (): Promise<void> => {
    setError('')
    player.stop()
    try {
      take.current = await record()
    } catch (e) {
      setError(`${S.recordNoMic}${e instanceof Error ? `（${e.message}）` : ''}`)
      return
    }
    pcm.current = null
    setSeconds(0)
    setLevel(0)
    setClipped(false)
    setPhase('recording')
  }

  const stop = async (): Promise<void> => {
    const t = take.current
    take.current = null
    if (t === null) {
      return
    }
    const wasClipped = t.peak() >= CLIP
    pcm.current = await t.stop()
    setSeconds(pcm.current.length / REFERENCE_RATE)
    setLevel(0)
    setClipped(wasClipped)
    setPhase('recorded')
  }

  const play = (): void => {
    if (pcm.current !== null) {
      player.stop()
      player.push(pcm.current)
      player.flush()
    }
  }

  const use = async (): Promise<void> => {
    if (pcm.current === null) {
      return
    }
    player.stop()
    setError('')
    setPhase('saving')
    const why = await window.yoai.reader.useRecording(toWav(pcm.current))
    setPhase('recorded')
    if (why.length > 0) {
      setError(why)
      return
    }
    onUsed()
  }

  const tooShort = seconds < MIN_SECONDS
  const clock = `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, '0')}`

  return (
    <div className="card recorder">
      <div className="field">
        <label>{S.recordTitle}</label>
        <p className="recorder-script">{S.recordScript}</p>
        <span className="field-note">{S.recordNote}</span>
      </div>

      <div className="row">
        <span className="row-label">
          <span className="recorder-clock">{clock}</span>
          {phase === 'recording' && (
            <span className="recorder-meter">
              <span style={{ width: `${Math.min(100, level * 400)}%` }} />
            </span>
          )}
        </span>
        {phase === 'recording' ? (
          <button className="btn btn-primary" onClick={() => void stop()}>
            {S.recordStop}
          </button>
        ) : (
          <>
            {phase !== 'idle' && (
              <button className="btn" disabled={phase === 'saving'} onClick={play}>
                {S.recordPlay}
              </button>
            )}
            <button className="btn" disabled={phase === 'saving'} onClick={() => void start()}>
              {phase === 'idle' ? S.recordStart : S.recordAgain}
            </button>
            {phase !== 'idle' && (
              <button
                className="btn btn-primary"
                disabled={phase === 'saving' || tooShort}
                onClick={() => void use()}
              >
                {phase === 'saving' ? S.recordSaving : S.recordUse}
              </button>
            )}
          </>
        )}
        <button className="btn" disabled={phase === 'saving'} onClick={onClose}>
          {S.settingsCancel}
        </button>
      </div>

      {(phase === 'recorded' || phase === 'saving') && tooShort && (
        <span className="field-note" data-tone="danger">
          {S.recordTooShort}
        </span>
      )}
      {clipped && (
        <span className="field-note" data-tone="danger">
          {S.recordClipped}
        </span>
      )}
      {error.length > 0 && (
        <span className="field-note" data-tone="danger">
          {error}
        </span>
      )}
    </div>
  )
}
