// WAV helpers: 24-bit PCM mono files, one per recorded channel.

export const BIT_DEPTH = 24;
export const HEADER_BYTES = 44;

function writeStr(view, offset, str) {
  for (let i = 0; i < str.length; i++) view.setUint8(offset + i, str.charCodeAt(i));
}

export const BEXT_BYTES = 602; // BWF bext chunk body (version 1, no coding history)

// Header bytes before the samples: 44, or 654 with a bext chunk.
export function headerBytes(bext) {
  return bext ? HEADER_BYTES + 8 + BEXT_BYTES : HEADER_BYTES;
}

// bext: optional Broadcast Wave info { description, originator, date
// ('yyyy-mm-dd'), time ('hh:mm:ss'), timeReference (samples since midnight of
// the first sample) }. Without it the header is a plain 44-byte WAV header.
export function wavHeader({ sampleRate, channels = 1, bitDepth = BIT_DEPTH, dataBytes = 0, bext = null }) {
  const size = headerBytes(bext);
  const buf = new ArrayBuffer(size);
  const v = new DataView(buf);
  const blockAlign = (channels * bitDepth) / 8;
  writeStr(v, 0, 'RIFF');
  v.setUint32(4, size - 8 + dataBytes, true);
  writeStr(v, 8, 'WAVE');
  writeStr(v, 12, 'fmt ');
  v.setUint32(16, 16, true);
  v.setUint16(20, 1, true); // PCM
  v.setUint16(22, channels, true);
  v.setUint32(24, sampleRate, true);
  v.setUint32(28, sampleRate * blockAlign, true);
  v.setUint16(32, blockAlign, true);
  v.setUint16(34, bitDepth, true);
  if (bext) writeBext(v, 36, bext);
  writeStr(v, size - 8, 'data');
  v.setUint32(size - 4, dataBytes, true);
  return new Uint8Array(buf);
}

function writeField(v, offset, len, str) {
  const s = String(str || '').replace(/[^\x20-\x7e]/g, '?').slice(0, len);
  writeStr(v, offset, s); // the rest stays zero
}

function writeBext(v, at, b) {
  writeStr(v, at, 'bext');
  v.setUint32(at + 4, BEXT_BYTES, true);
  const o = at + 8;
  writeField(v, o, 256, b.description);
  writeField(v, o + 256, 32, b.originator);
  writeField(v, o + 288, 32, b.originatorReference);
  writeField(v, o + 320, 10, b.date);
  writeField(v, o + 330, 8, b.time);
  v.setBigUint64(o + 338, BigInt(Math.max(0, Math.round(b.timeReference || 0))), true);
  v.setUint16(o + 346, 1, true); // version
  // UMID, loudness fields and reserved bytes stay zero.
}

// Float samples (-1..1) to little-endian 24-bit integers.
export function floatTo24(f32) {
  const out = new Uint8Array(f32.length * 3);
  for (let i = 0, j = 0; i < f32.length; i++, j += 3) {
    const s = f32[i] > 1 ? 1 : f32[i] < -1 ? -1 : f32[i];
    const x = s < 0 ? Math.round(s * 0x800000) : Math.round(s * 0x7fffff);
    out[j] = x & 0xff;
    out[j + 1] = (x >> 8) & 0xff;
    out[j + 2] = (x >> 16) & 0xff;
  }
  return out;
}

// Build a complete mono WAV (as byte arrays) from a list of Float32Array chunks.
export function encodeWavParts(chunks, sampleRate, bext = null) {
  const parts = chunks.map(floatTo24);
  const dataBytes = parts.reduce((n, p) => n + p.length, 0);
  return [wavHeader({ sampleRate, dataBytes, bext }), ...parts];
}

// Max absolute value of each `step`-sample window, for waveform drawing.
export function peaksOf(f32, step) {
  const out = new Float32Array(Math.ceil(f32.length / step));
  for (let p = 0; p < out.length; p++) {
    let m = 0;
    const end = Math.min(f32.length, (p + 1) * step);
    for (let i = p * step; i < end; i++) {
      const a = f32[i] < 0 ? -f32[i] : f32[i];
      if (a > m) m = a;
    }
    out[p] = m;
  }
  return out;
}
