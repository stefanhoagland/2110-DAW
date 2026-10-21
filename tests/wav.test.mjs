import test from 'node:test';
import assert from 'node:assert/strict';
import { wavHeader, floatTo24, encodeWavParts, peaksOf, headerBytes } from '../js/wav.js';

test('header describes 24-bit mono PCM', () => {
  const h = new DataView(wavHeader({ sampleRate: 48000, dataBytes: 300 }).buffer);
  const str = (o) => String.fromCharCode(...new Uint8Array(h.buffer, o, 4));
  assert.equal(str(0), 'RIFF');
  assert.equal(h.getUint32(4, true), 336);
  assert.equal(str(8), 'WAVE');
  assert.equal(h.getUint16(22, true), 1);
  assert.equal(h.getUint32(24, true), 48000);
  assert.equal(h.getUint32(28, true), 144000);
  assert.equal(h.getUint16(32, true), 3);
  assert.equal(h.getUint16(34, true), 24);
  assert.equal(h.getUint32(40, true), 300);
});

test('float to 24-bit conversion, with clipping', () => {
  const b = floatTo24(new Float32Array([0, 1, -1, 2, 0.5]));
  const read = (i) => ((b[i * 3] | (b[i * 3 + 1] << 8) | (b[i * 3 + 2] << 16)) << 8) >> 8;
  assert.deepEqual([0, 1, 2, 3, 4].map(read), [0, 0x7fffff, -0x800000, 0x7fffff, Math.round(0.5 * 0x7fffff)]);
});

test('encodeWavParts concatenates chunks with correct size', () => {
  const parts = encodeWavParts([new Float32Array(10), new Float32Array(5)], 44100);
  const total = parts.reduce((n, p) => n + p.length, 0);
  assert.equal(total, 44 + 15 * 3);
  assert.equal(new DataView(parts[0].buffer).getUint32(40, true), 45);
});

test('peaksOf takes max absolute per window', () => {
  assert.deepEqual(Array.from(peaksOf(new Float32Array([0.1, -0.5, 0.2, 0.3, -0.9]), 2)), [0.5, 0.30000001192092896, 0.8999999761581421]);
});

// Read a WAV file's chunks back.
function parseWav(bytes) {
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const str = (o, n) => String.fromCharCode(...bytes.subarray(o, o + n));
  const chunks = {};
  let o = 12;
  while (o + 8 <= bytes.length) {
    const id = str(o, 4);
    const size = v.getUint32(o + 4, true);
    chunks[id] = { offset: o + 8, size };
    if (id === 'data') break;
    o += 8 + size + (size & 1);
  }
  return { riff: str(0, 4), riffSize: v.getUint32(4, true), wave: str(8, 4), chunks, v, str };
}

test('without timecode the header is the plain 44-byte one', () => {
  const h = wavHeader({ sampleRate: 48000, dataBytes: 300 });
  assert.equal(h.length, 44);
  assert.deepEqual(h, wavHeader({ sampleRate: 48000, dataBytes: 300, bext: null }));
  assert.deepEqual(Object.keys(parseWav(h).chunks), ['fmt ', 'data']);
});

test('bext chunk carries the timecode stamp, before the data chunk', () => {
  const bext = {
    description: 'sTC=01:00:00:00 fps=25',
    originator: 'Soundbending Live',
    date: '2026-10-21',
    time: '14:03:09',
    timeReference: 3600 * 96000 * 24 + 12345, // past 32 bits
  };
  const parts = encodeWavParts([new Float32Array(10), new Float32Array(5)], 96000, bext);
  const file = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) { file.set(p, o); o += p.length; }
  const w = parseWav(file);
  assert.equal(w.riff, 'RIFF');
  assert.equal(w.wave, 'WAVE');
  assert.equal(w.riffSize, file.length - 8);
  assert.equal(headerBytes(bext), 654);
  assert.deepEqual(Object.keys(w.chunks), ['fmt ', 'bext', 'data']);
  assert.equal(w.chunks.bext.size, 602);
  assert.equal(w.chunks.data.offset, 654);
  assert.equal(w.chunks.data.size, 45);
  assert.equal(w.chunks.data.offset + w.chunks.data.size, file.length);
  const b = w.chunks.bext.offset;
  const field = (off, len) => w.str(b + off, len).replace(/\0+$/, '');
  assert.equal(field(0, 256), bext.description);
  assert.equal(field(256, 32), 'Soundbending Live');
  assert.equal(field(320, 10), '2026-10-21');
  assert.equal(field(330, 8), '14:03:09');
  assert.equal(w.v.getBigUint64(b + 338, true), BigInt(bext.timeReference));
  assert.equal(w.v.getUint16(b + 346, true), 1);
  assert.equal(w.v.getUint32(24, true), 96000);
});

test('rewriting the header on close keeps the bext chunk and data offsets', () => {
  // As DiskWriter does: a placeholder header first, the final one written over it.
  const bext = { description: '', timeReference: 0 };
  const placeholder = wavHeader({ sampleRate: 48000, bext });
  const data = new Uint8Array(30).fill(7);
  bext.description = 'sTC=10:00:00;00 fps=29.97DF';
  bext.timeReference = 1726272000;
  const final = wavHeader({ sampleRate: 48000, dataBytes: data.length, bext });
  assert.equal(final.length, placeholder.length);
  const file = new Uint8Array(final.length + data.length);
  file.set(placeholder); file.set(data, placeholder.length); file.set(final, 0);
  const w = parseWav(file);
  assert.equal(w.chunks.data.offset, 654);
  assert.equal(w.chunks.data.size, 30);
  assert.equal(w.riffSize, file.length - 8);
  assert.equal(w.v.getBigUint64(w.chunks.bext.offset + 338, true), 1726272000n);
});
