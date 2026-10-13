/**
 * A take as the speech engine reads it: a RIFF/WAVE file, 16-bit mono PCM.
 *
 * The engine takes a reference voice in exactly this shape at 24 kHz and
 * refuses anything else rather than converting it, so the recorder captures
 * at that rate and this only adds the header. A port of toWav in
 * examples/desktop/src/renderer/src/recorder.ts.
 */
import { common } from '@kit.AbilityKit';
import fs from '@ohos.file.fs';

/** `pcm` (16-bit samples, as recorded) behind a 44-byte header. */
export function toWav(pcm: ArrayBuffer, rate: number): ArrayBuffer {
  const bytes: ArrayBuffer = new ArrayBuffer(44 + pcm.byteLength);
  const v: DataView = new DataView(bytes);
  const tag = (at: number, s: string): void => {
    for (let i = 0; i < 4; i++) {
      v.setUint8(at + i, s.charCodeAt(i));
    }
  };
  tag(0, 'RIFF');
  v.setUint32(4, 36 + pcm.byteLength, true);
  tag(8, 'WAVE');
  tag(12, 'fmt ');
  v.setUint32(16, 16, true);
  v.setUint16(20, 1, true);          // PCM
  v.setUint16(22, 1, true);          // mono
  v.setUint32(24, rate, true);
  v.setUint32(28, rate * 2, true);   // byte rate
  v.setUint16(32, 2, true);          // block align
  v.setUint16(34, 16, true);         // bits
  tag(36, 'data');
  v.setUint32(40, pcm.byteLength, true);
  new Uint8Array(bytes, 44).set(new Uint8Array(pcm));
  return bytes;
}

/**
 * Keep a take under filesDir/recordings/, one file per take, named by when it
 * was made, and return its path. Kept as the recording rather than only as its
 * embedding, so it can be extracted again for the other model size.
 */
export function saveRecording(ctx: common.Context, pcm: ArrayBuffer, rate: number): string {
  const dir: string = ctx.filesDir + '/recordings';
  fs.mkdirSync(dir, true);
  const d: Date = new Date();
  const two = (n: number): string => (n < 10 ? '0' : '') + n;
  const stamp: string = d.getFullYear() + two(d.getMonth() + 1) + two(d.getDate()) + '-' +
    two(d.getHours()) + two(d.getMinutes()) + two(d.getSeconds());
  const path: string = dir + '/recording-' + stamp + '.wav';
  const out: fs.File = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
  try {
    fs.writeSync(out.fd, toWav(pcm, rate));
  } finally {
    fs.closeSync(out);
  }
  return path;
}
