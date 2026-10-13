/**
 * The microphone, for a voice conversation: 16-bit mono at 16 kHz, which is
 * what recognition and the echo canceller take (src/yoai.h).
 *
 * At 16 kHz from the start, by asking for an AudioContext at that rate and
 * letting the browser bring the device to it -- the same resampler
 * recorder.ts relies on, once, at the input.
 *
 * **Raw: the browser's echo cancellation, noise suppression and gain control
 * are all off.** Its canceller left enough of the reply in the microphone for
 * recognition to write it back down as the user's words, as Android's did, so
 * the core's canceller does the job, and it needs the signal nobody touched.
 * Gain control would also ride the level up in every pause, which is exactly
 * where the speech detector reads the room.
 */

export const MIC_RATE = 16000

/** 512 frames: a block every 32ms. Smaller blocks pair the reference closer. */
const BLOCK = 512

export interface Mic {
  /** Let go of the device. */
  close(): void
}

/**
 * Open the microphone and hand `onBlock` every block as it is captured.
 * Rejects when there is no microphone or the user refused it.
 */
export async function openMic(onBlock: (pcm: Int16Array) => void): Promise<Mic> {
  const stream = await navigator.mediaDevices.getUserMedia({
    audio: {
      channelCount: 1,
      echoCancellation: false,
      noiseSuppression: false,
      autoGainControl: false
    }
  })
  const ctx = new AudioContext({ sampleRate: MIC_RATE })
  const source = ctx.createMediaStreamSource(stream)
  // A ScriptProcessor, as in recorder.ts: no worklet module can be loaded.
  const tap = ctx.createScriptProcessor(BLOCK, 1, 1)
  tap.onaudioprocess = (e): void => {
    const f = e.inputBuffer.getChannelData(0)
    const pcm = new Int16Array(f.length)
    for (let i = 0; i < f.length; i++) {
      const v = Math.max(-1, Math.min(1, f[i]))
      pcm[i] = v < 0 ? v * 32768 : v * 32767
    }
    onBlock(pcm)
  }
  source.connect(tap)
  tap.connect(ctx.destination)
  if (ctx.state === 'suspended') {
    void ctx.resume()
  }
  return {
    close(): void {
      tap.onaudioprocess = null
      source.disconnect()
      tap.disconnect()
      for (const track of stream.getTracks()) {
        track.stop()
      }
      void ctx.close()
    }
  }
}

/** Blocks laid end to end. */
export function joinBlocks(blocks: Int16Array[]): Int16Array {
  const out = new Int16Array(blocks.reduce((n, b) => n + b.length, 0))
  let at = 0
  for (const b of blocks) {
    out.set(b, at)
    at += b.length
  }
  return out
}
