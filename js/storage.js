// Where recorded audio goes.
// DiskWriter streams each channel straight to a WAV file in a folder the user
// picked (Chrome/Edge), so long multitrack takes don't fill up memory.
// MemoryWriter keeps samples in memory and exports WAVs as downloads.

import { wavHeader, floatTo24, encodeWavParts, BIT_DEPTH } from './wav.js';

export const canUseFolders = typeof window !== 'undefined' && 'showDirectoryPicker' in window;

export async function pickFolder() {
  return window.showDirectoryPicker({ id: 'daw-2110-projects', mode: 'readwrite' });
}

export class DiskWriter {
  static async create(dir, fileName, sampleRate) {
    const handle = await dir.getFileHandle(fileName, { create: true });
    const stream = await handle.createWritable();
    const w = new DiskWriter(handle, stream, sampleRate);
    w.queue = stream.write(wavHeader({ sampleRate })); // placeholder sizes, fixed on close
    return w;
  }

  constructor(handle, stream, sampleRate) {
    this.handle = handle;
    this.stream = stream;
    this.sampleRate = sampleRate;
    this.dataBytes = 0;
    this.error = null;
  }

  write(f32) {
    const bytes = floatTo24(f32);
    this.dataBytes += bytes.length;
    this.queue = this.queue
      .then(() => this.stream.write(bytes))
      .catch((e) => { this.error = e; });
  }

  async close() {
    await this.queue;
    const header = wavHeader({ sampleRate: this.sampleRate, dataBytes: this.dataBytes });
    await this.stream.write({ type: 'write', position: 0, data: header });
    await this.stream.close();
    if (this.error) throw this.error;
  }

  async readArrayBuffer() {
    const file = await this.handle.getFile();
    return file.arrayBuffer();
  }
}

export class MemoryWriter {
  constructor(fileName, sampleRate) {
    this.fileName = fileName;
    this.sampleRate = sampleRate;
    this.chunks = [];
  }

  write(f32) {
    this.chunks.push(f32);
  }

  async close() {}

  toBlob() {
    return new Blob(encodeWavParts(this.chunks, this.sampleRate), { type: 'audio/wav' });
  }

  toAudioBuffer(ctx) {
    const length = this.chunks.reduce((n, c) => n + c.length, 0);
    const buf = ctx.createBuffer(1, Math.max(1, length), this.sampleRate);
    const out = buf.getChannelData(0);
    let o = 0;
    for (const c of this.chunks) { out.set(c, o); o += c.length; }
    return buf;
  }
}

export function downloadBlob(blob, fileName) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = fileName;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}

export function safeName(s) {
  return String(s).replace(/[^\w\- ]+/g, '').trim().replace(/\s+/g, '_') || 'Track';
}

export { BIT_DEPTH };
