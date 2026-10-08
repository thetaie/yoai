/**
 * Recording a reference voice from the microphone.
 *
 * What the core takes is exact: mono, 16-bit, 24000 Hz, and a file that is not
 * is refused rather than converted, because the core carries no resampler. So
 * the conversion happens here, and by the browser's own resampler rather than
 * one written for the purpose: the microphone is captured at whatever rate the
 * device runs, and the take is rendered to 24 kHz through an
 * OfflineAudioContext when it ends.
 *
 * **Echo cancellation, noise suppression and automatic gain are off.** They are
 * made for calls, and each one reshapes the voice -- the gain control most
 * audibly, riding the level up in every pause. A reference voice is a
 * description of how someone sounds, and it should be the unprocessed sound.
 */

/** What the engine takes, and so what a take is rendered to. */
export const REFERENCE_RATE = 24000

export interface Take {
  /** Stop and hand back the take: mono float at REFERENCE_RATE. */
  stop(): Promise<Float32Array>
  /** Stop and throw the take away. */
  cancel(): void
  /** Seconds recorded so far. */
  seconds(): number
  /** The level of the last block, 0..1 RMS, for a meter. */
  level(): number
  /** The loudest sample so far; at 1 the take is clipped. */
  peak(): number
}

export async function record(): Promise<Take> {
  const stream = await navigator.mediaDevices.getUserMedia({
    audio: {
      channelCount: 1,
      echoCancellation: false,
      noiseSuppression: false,
      autoGainControl: false
    }
  })
  const ctx = new AudioContext()
  const rate = ctx.sampleRate
  const source = ctx.createMediaStreamSource(stream)
  // A ScriptProcessor rather than an AudioWorklet: a worklet is a module loaded
  // by URL, and this page's policy loads nothing it did not ship with. This
  // runs on the page thread, which for a take of seconds is no burden.
  const tap = ctx.createScriptProcessor(4096, 1, 1)

  const blocks: Float32Array[] = []
  let samples = 0
  let lastLevel = 0
  let loudest = 0
  tap.onaudioprocess = (e) => {
    const input = e.inputBuffer.getChannelData(0)
    blocks.push(new Float32Array(input))
    samples += input.length
    let sum = 0
    for (let i = 0; i < input.length; i++) {
      const s = input[i]
      sum += s * s
      const a = Math.abs(s)
      if (a > loudest) {
        loudest = a
      }
    }
    lastLevel = Math.sqrt(sum / input.length)
  }
  source.connect(tap)
  // A processor only runs while connected to the output. Its own output is
  // left silent, so nothing is heard.
  tap.connect(ctx.destination)

  const release = (): void => {
    tap.onaudioprocess = null
    source.disconnect()
    tap.disconnect()
    for (const track of stream.getTracks()) {
      track.stop()
    }
    void ctx.close()
  }

  return {
    async stop() {
      release()
      const raw = new Float32Array(samples)
      let at = 0
      for (const b of blocks) {
        raw.set(b, at)
        at += b.length
      }
      if (rate === REFERENCE_RATE || raw.length === 0) {
        return raw
      }
      const frames = Math.round((raw.length * REFERENCE_RATE) / rate)
      const offline = new OfflineAudioContext(1, frames, REFERENCE_RATE)
      const buffer = offline.createBuffer(1, raw.length, rate)
      buffer.copyToChannel(raw, 0)
      const play = offline.createBufferSource()
      play.buffer = buffer
      play.connect(offline.destination)
      play.start()
      const rendered = await offline.startRendering()
      return new Float32Array(rendered.getChannelData(0))
    },
    cancel() {
      release()
    },
    seconds: () => samples / rate,
    level: () => lastLevel,
    peak: () => loudest
  }
}

/** A take as the core reads it: a RIFF/WAVE file, 16-bit mono PCM. */
export function toWav(pcm: Float32Array, rate: number = REFERENCE_RATE): Uint8Array {
  const bytes = new Uint8Array(44 + pcm.length * 2)
  const v = new DataView(bytes.buffer)
  const tag = (at: number, s: string): void => {
    for (let i = 0; i < 4; i++) {
      v.setUint8(at + i, s.charCodeAt(i))
    }
  }
  tag(0, 'RIFF')
  v.setUint32(4, 36 + pcm.length * 2, true)
  tag(8, 'WAVE')
  tag(12, 'fmt ')
  v.setUint32(16, 16, true)
  v.setUint16(20, 1, true)          // PCM
  v.setUint16(22, 1, true)          // mono
  v.setUint32(24, rate, true)
  v.setUint32(28, rate * 2, true)   // byte rate
  v.setUint16(32, 2, true)          // block align
  v.setUint16(34, 16, true)         // bits
  tag(36, 'data')
  v.setUint32(40, pcm.length * 2, true)
  for (let i = 0; i < pcm.length; i++) {
    const s = Math.max(-1, Math.min(1, pcm[i]))
    v.setInt16(44 + i * 2, Math.round(s * 32767), true)
  }
  return bytes
}
