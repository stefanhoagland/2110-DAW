import test from 'node:test';
import assert from 'node:assert/strict';
import { wavHeader, floatTo24, encodeWavParts, peaksOf } from '../js/wav.js';

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
