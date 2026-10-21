// Timecode helpers with no browser dependencies, so they run in the audio
// worklet, on the main thread and in node tests:
// - LtcDecoder: SMPTE 12M linear timecode (biphase-mark audio) to frames.
// - LtcRateEstimator: frame rate from the spacing of decoded frames.
// - MtcParser: MIDI timecode (quarter frames and full-frame sysex).
// - frame counting (including 29.97 drop-frame) and labels.
// - encodeLtc: a synthetic LTC signal, for tests.

export const RATES = {
  '23.976': { id: '23.976', name: '23.976', nominal: 24, fps: 24000 / 1001, df: false },
  '24': { id: '24', name: '24', nominal: 24, fps: 24, df: false },
  '25': { id: '25', name: '25', nominal: 25, fps: 25, df: false },
  '29.97': { id: '29.97', name: '29.97', nominal: 30, fps: 30000 / 1001, df: false },
  '29.97DF': { id: '29.97DF', name: '29.97 DF', nominal: 30, fps: 30000 / 1001, df: true },
  '30': { id: '30', name: '30', nominal: 30, fps: 30, df: false },
};

// Frames in 24 hours at this rate.
export function framesPerDay(rate) {
  return rate.df ? 2589408 : rate.nominal * 86400;
}

// Timecode label {h, m, s, f} to a frame count since midnight.
export function tcToFrames({ h, m, s, f }, rate) {
  const n = ((h * 60 + m) * 60 + s) * rate.nominal + f;
  if (!rate.df) return n;
  const minutes = h * 60 + m;
  return n - 2 * (minutes - Math.floor(minutes / 10));
}

// Frame count since midnight (whole frames, wraps at 24 h) to a label.
export function framesToTc(frames, rate) {
  const day = framesPerDay(rate);
  let n = ((Math.floor(frames) % day) + day) % day;
  if (rate.df) {
    const tens = Math.floor(n / 17982); // frames per ten minutes
    const rest = n % 17982;
    n += 18 * tens + (rest >= 2 ? 2 * Math.floor((rest - 2) / 1798) : 0);
  }
  const fps = rate.nominal;
  return {
    h: Math.floor(n / (fps * 3600)),
    m: Math.floor(n / (fps * 60)) % 60,
    s: Math.floor(n / fps) % 60,
    f: n % fps,
  };
}

export function formatTc({ h, m, s, f }, df = false) {
  const p = (x) => String(x).padStart(2, '0');
  return `${p(h)}:${p(m)}:${p(s)}${df ? ';' : ':'}${p(f)}`;
}

// "25 fps", "29.97 fps DF"
export function rateText(rate) {
  return rate.df ? `${rate.name.replace(' DF', '')} fps DF` : `${rate.name} fps`;
}

// Snap a measured frames-per-second to a standard rate. The drop-frame flag
// from LTC picks 29.97 DF. Returns null if nothing is close.
export function snapRate(fps, dropFrame = false) {
  const list = ['23.976', '24', '25', '29.97', '30'].map((k) => RATES[k]);
  let best = null;
  let err = Infinity;
  for (const r of list) {
    const e = Math.abs(fps / r.fps - 1);
    if (e < err) { err = e; best = r; }
  }
  if (err > 0.03) return null;
  if (dropFrame && best.nominal === 30) return RATES['29.97DF'];
  return best;
}

// Samples since midnight of a moment `frames` (fractional) frames into the day.
export function timeReference(frames, rate, sampleRate) {
  return Math.round((frames / rate.fps) * sampleRate);
}

// ---------- LTC decoding ----------

const SYNC = 0x3ffd; // bits 64..79 in the order they are sent

function bcd(bits, start, count) {
  let v = 0;
  for (let i = 0; i < count; i++) v |= bits[start + i] << i;
  return v;
}

// Feed it audio of one channel; it calls onFrame({ h, m, s, f, df, pos }) for
// each frame decoded. `pos` is the sample position (fractional, in the same
// count as the `start` given to process) where the frame's sync word ended,
// that is where the next frame begins. So at `pos` the time is label + 1 frame.
export class LtcDecoder {
  constructor(sampleRate, onFrame) {
    this.sampleRate = sampleRate;
    this.onFrame = onFrame;
    // Bit periods from 30 fps (shortest) to 23.976 fps (longest), with margin.
    this.minPeriod = sampleRate / (80 * 34);
    this.maxPeriod = sampleRate / (80 * 21);
    this.reset();
  }

  reset() {
    this.period = this.sampleRate / (80 * 25); // first guess, works for all rates
    this.high = false;
    this.lastCross = null; // position of the last zero crossing
    this.prev = 0;
    this.env = 0;
    this.half = false; // waiting for the second half of a 1
    this.bits = new Uint8Array(80);
    this.bitPos = 0; // ring buffer write index
    this.bitCount = 0;
    this.sync = 0;
  }

  process(samples, start = 0) {
    const decay = Math.exp(-1 / (0.05 * this.sampleRate));
    for (let i = 0; i < samples.length; i++) {
      const x = samples[i];
      const a = x < 0 ? -x : x;
      this.env = a > this.env ? a : this.env * decay;
      const thr = Math.max(0.002, this.env * 0.15); // hysteresis
      let crossed = false;
      let level = 0;
      if (this.high && x < -thr) { crossed = true; level = -thr; this.high = false; }
      else if (!this.high && x > thr) { crossed = true; level = thr; this.high = true; }
      if (crossed) {
        // Where the signal passed the threshold, between the previous sample and this one.
        const d = x - this.prev;
        const frac = d !== 0 ? Math.min(1, Math.max(0, (level - this.prev) / d)) : 1;
        this.crossing(start + i - 1 + frac);
      }
      this.prev = x;
    }
  }

  crossing(pos) {
    const last = this.lastCross;
    this.lastCross = pos;
    if (last === null) return;
    const dt = pos - last;
    if (dt > this.maxPeriod * 1.6 || dt < this.minPeriod * 0.3) { // silence or noise: start over
      this.half = false;
      this.sync = 0;
      this.bitCount = 0;
      return;
    }
    if (dt < this.period * 0.75) {
      this.adapt(dt * 2);
      if (this.half) { this.half = false; this.bit(1, pos); } else this.half = true;
    } else {
      this.adapt(dt);
      if (this.half) { this.half = false; this.sync = 0; this.bitCount = 0; } // lost bit sync
      this.bit(0, pos);
    }
  }

  adapt(p) {
    const v = this.period * 0.9 + p * 0.1;
    this.period = Math.min(this.maxPeriod, Math.max(this.minPeriod, v));
  }

  bit(b, pos) {
    this.bits[this.bitPos] = b;
    this.bitPos = (this.bitPos + 1) % 80;
    if (this.bitCount < 80) this.bitCount++;
    this.sync = ((this.sync << 1) | b) & 0xffff;
    if (this.sync !== SYNC || this.bitCount < 80) return;
    const bits = new Uint8Array(64);
    for (let k = 0; k < 64; k++) bits[k] = this.bits[(this.bitPos + k) % 80];
    const fu = bcd(bits, 0, 4), ft = bcd(bits, 8, 2);
    const su = bcd(bits, 16, 4), st = bcd(bits, 24, 3);
    const mu = bcd(bits, 32, 4), mt = bcd(bits, 40, 3);
    const hu = bcd(bits, 48, 4), ht = bcd(bits, 56, 2);
    if (fu > 9 || su > 9 || mu > 9 || hu > 9 || st > 5 || mt > 5) return;
    const tc = { h: ht * 10 + hu, m: mt * 10 + mu, s: st * 10 + su, f: ft * 10 + fu, df: !!bits[10], pos };
    if (tc.h > 23 || tc.f > 29) return;
    this.onFrame(tc);
  }
}

// Frame rate from the positions of decoded LTC frames, averaged over the last
// ~2 seconds. Starts over when frames stop arriving evenly.
export class LtcRateEstimator {
  constructor(sampleRate, windowSec = 2) {
    this.sampleRate = sampleRate;
    this.window = windowSec * sampleRate;
    this.list = [];
    this.rate = null;
  }

  reset() {
    this.list = [];
    this.rate = null;
  }

  // Returns the current rate (a RATES entry) or null while still unsure.
  push(frame) {
    const l = this.list;
    const last = l[l.length - 1];
    if (last) {
      const dt = frame.pos - last.pos;
      const expect = l.length > 1 ? (last.pos - l[0].pos) / (l.length - 1) : null;
      const bad = dt <= 0 || dt > this.sampleRate / 20 || (expect && Math.abs(dt / expect - 1) > 0.3);
      if (bad) l.length = 0;
    }
    l.push(frame);
    while (l.length > 2 && frame.pos - l[0].pos > this.window) l.shift();
    if (l.length >= 5) {
      const spf = (frame.pos - l[0].pos) / (l.length - 1);
      const r = snapRate(this.sampleRate / spf, frame.df);
      if (r) this.rate = r;
    }
    return this.rate;
  }
}

// ---------- LTC encoding (test signals) ----------

function ltcBits({ h, m, s, f }, df) {
  const bits = new Array(80).fill(0);
  const put = (start, count, v) => { for (let i = 0; i < count; i++) bits[start + i] = (v >> i) & 1; };
  put(0, 4, f % 10); put(8, 2, Math.floor(f / 10));
  bits[10] = df ? 1 : 0;
  put(16, 4, s % 10); put(24, 3, Math.floor(s / 10));
  put(32, 4, m % 10); put(40, 3, Math.floor(m / 10));
  put(48, 4, h % 10); put(56, 2, Math.floor(h / 10));
  [0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1].forEach((b, i) => (bits[64 + i] = b));
  return bits;
}

// A test LTC signal: biphase-mark, the level flips at every bit boundary and
// mid-bit for a 1. `offset` samples of steady level come first.
export function encodeLtc({ rate, start, seconds, sampleRate, amp = 0.5, noise = 0.01, offset = 0 }) {
  const out = new Float32Array(Math.ceil(seconds * sampleRate) + offset);
  const bitLen = sampleRate / rate.fps / 80;
  let level = amp;
  let n = tcToFrames(start, rate);
  let t = offset; // position of the current bit boundary, in samples
  let i = 0;
  const fill = (until) => { for (; i < out.length && i < until; i++) out[i] = level; };
  fill(offset);
  let seed = 1;
  const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647 - 0.5) * 2;
  while (t < out.length) {
    const bits = ltcBits(framesToTc(n, rate), rate.df);
    for (const b of bits) {
      fill(t); level = -level;
      if (b) { fill(t + bitLen / 2); level = -level; }
      t += bitLen;
    }
    n++;
  }
  fill(out.length);
  for (let k = 0; k < out.length; k++) out[k] += noise * rnd();
  return out;
}

// ---------- MIDI timecode ----------

const MTC_RATES = ['24', '25', '29.97DF', '30'];

// Feed it MIDI messages (bytes, time in ms). It returns an anchor
// { h, m, s, f, rate, frames, time } when a full time is known: `frames` is
// the frame count since midnight at `time`. Quarter frames give one every two
// frames (on piece 7, adding the 2 frames the message took); full-frame sysex
// gives one right away.
export class MtcParser {
  constructor() {
    this.pieces = new Array(8).fill(0);
    this.seen = 0; // bit mask of pieces received in order since piece 0
    this.lastPiece = -1;
  }

  message(data, time) {
    if (data[0] === 0xf1 && data.length >= 2) {
      const piece = (data[1] >> 4) & 7;
      const value = data[1] & 0x0f;
      if (piece === 0) this.seen = 0;
      else if (piece !== this.lastPiece + 1) this.seen = 0; // out of order (or backwards)
      this.lastPiece = piece;
      this.pieces[piece] = value;
      this.seen |= 1 << piece;
      if (piece !== 7 || this.seen !== 0xff) return null;
      const p = this.pieces;
      const rate = RATES[MTC_RATES[(p[7] >> 1) & 3]];
      const tc = { h: ((p[7] & 1) << 4) | p[6], m: ((p[5] & 3) << 4) | p[4], s: ((p[3] & 3) << 4) | p[2], f: ((p[1] & 1) << 4) | p[0] };
      return { ...tc, rate, frames: tcToFrames(tc, rate) + 2, time, quarter: true };
    }
    // Full frame: F0 7F <dev> 01 01 hh mm ss ff F7
    if (data[0] === 0xf0 && data.length >= 10 && data[1] === 0x7f && data[3] === 0x01 && data[4] === 0x01) {
      const rate = RATES[MTC_RATES[(data[5] >> 5) & 3]];
      const tc = { h: data[5] & 0x1f, m: data[6] & 0x3f, s: data[7] & 0x3f, f: data[8] & 0x1f };
      this.seen = 0;
      this.lastPiece = -1;
      return { ...tc, rate, frames: tcToFrames(tc, rate), time, quarter: false };
    }
    return null;
  }
}
