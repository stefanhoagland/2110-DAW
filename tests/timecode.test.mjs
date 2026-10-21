import test from 'node:test';
import assert from 'node:assert/strict';
import {
  RATES, tcToFrames, framesToTc, formatTc, snapRate, timeReference,
  LtcDecoder, LtcRateEstimator, MtcParser, encodeLtc,
} from '../js/timecode.js';

function decodeAll(signal, sampleRate, block = 128) {
  const frames = [];
  const dec = new LtcDecoder(sampleRate, (f) => frames.push(f));
  for (let p = 0; p < signal.length; p += block) dec.process(signal.subarray(p, p + block), p);
  return frames;
}

const cases = [
  { id: '25', start: { h: 1, m: 0, s: 0, f: 0 } },
  { id: '24', start: { h: 10, m: 59, s: 58, f: 20 } },
  { id: '29.97DF', start: { h: 0, m: 0, s: 59, f: 20 } }, // crosses a dropped minute
  { id: '30', start: { h: 23, m: 59, s: 58, f: 0 } }, // wraps at midnight
  { id: '23.976', start: { h: 2, m: 3, s: 4, f: 5 } },
];

for (const sr of [48000, 44100]) {
  for (const c of cases) {
    test(`LTC ${c.id} fps at ${sr} Hz decodes labels, positions and rate`, () => {
      const rate = RATES[c.id];
      const offset = 37;
      const signal = encodeLtc({ rate, start: c.start, seconds: 3, sampleRate: sr, offset });
      const frames = decodeAll(signal, sr);
      const total = Math.floor((3 * rate.fps));
      assert.ok(frames.length >= total - 2, `decoded ${frames.length} of ~${total} frames`);
      const n0 = tcToFrames(c.start, rate);
      const spf = sr / rate.fps;
      // The first frame may be missed while the decoder locks on; find which one we got.
      const k0 = Math.round((frames[0].pos - offset) / spf) - 1;
      assert.ok(k0 >= 0 && k0 <= 1);
      frames.forEach((f, j) => {
        const k = k0 + j;
        const want = framesToTc(n0 + k, rate);
        assert.equal(formatTc(f, f.df), formatTc(want, rate.df));
        assert.equal(f.df, rate.df);
        // The sync word ends at the end of the frame.
        assert.ok(Math.abs(f.pos - (offset + (k + 1) * spf)) < 1.5, `frame ${k} at ${f.pos}`);
      });
      const est = new LtcRateEstimator(sr);
      let r = null;
      for (const f of frames) r = est.push(f);
      assert.equal(r && r.id, rate.id);
    });
  }
}

test('LTC decodes an inverted, quiet, rounded and slightly fast signal', () => {
  const sr = 48000;
  const rate = { ...RATES['25'], fps: 25 * 1.01 }; // 1% fast
  const sig = encodeLtc({ rate, start: { h: 5, m: 6, s: 7, f: 8 }, seconds: 2, sampleRate: sr, amp: -0.05, noise: 0.002 });
  for (let i = 1, y = 0; i < sig.length; i++) sig[i] = y += 0.3 * (sig[i] - y); // rounded edges
  const frames = decodeAll(sig, sr, 100);
  assert.ok(frames.length > 45);
  assert.equal(formatTc(frames.at(-1)), formatTc(framesToTc(tcToFrames(frames[0], RATES['25']) + frames.length - 1, RATES['25'])));
  const est = new LtcRateEstimator(sr);
  let r = null;
  for (const f of frames) r = est.push(f);
  assert.equal(r.id, '25');
});

test('silence and noise decode nothing', () => {
  const sr = 48000;
  const noise = new Float32Array(sr * 2);
  let seed = 7;
  for (let i = 0; i < noise.length; i++) noise[i] = ((seed = (seed * 16807) % 2147483647) / 2147483647 - 0.5) * 0.6;
  assert.equal(decodeAll(new Float32Array(sr), sr).length, 0);
  assert.equal(decodeAll(noise, sr).length, 0);
});

test('drop-frame counting', () => {
  const df = RATES['29.97DF'];
  assert.equal(tcToFrames({ h: 0, m: 1, s: 0, f: 2 }, df), 1800);
  assert.equal(formatTc(framesToTc(1799, df), true), '00:00:59;29');
  assert.equal(formatTc(framesToTc(1800, df), true), '00:01:00;02');
  assert.equal(formatTc(framesToTc(17982, df), true), '00:10:00;00');
  assert.equal(tcToFrames({ h: 1, m: 0, s: 0, f: 0 }, df), 107892);
  for (const n of [0, 1, 1799, 1800, 17981, 17982, 107891, 107892, 2589407]) {
    assert.equal(tcToFrames(framesToTc(n, df), df), n);
  }
  assert.equal(formatTc(framesToTc(2589408, df), true), '00:00:00;00'); // wraps at 24 h
});

test('rate snapping and time reference', () => {
  assert.equal(snapRate(23.98).id, '23.976');
  assert.equal(snapRate(24.01).id, '24');
  assert.equal(snapRate(29.97).id, '29.97');
  assert.equal(snapRate(29.97, true).id, '29.97DF');
  assert.equal(snapRate(50), null);
  assert.equal(timeReference(tcToFrames({ h: 1, m: 0, s: 0, f: 0 }, RATES['25']), RATES['25'], 48000), 172800000);
  // 01:00:00;00 DF is 107892 frames at 30000/1001 fps = 3599.9964 s.
  assert.equal(timeReference(107892, RATES['29.97DF'], 48000), Math.round(107892 * 1001 / 30000 * 48000));
});

test('MTC quarter frames and full frame', () => {
  const p = new MtcParser();
  // 01:02:03:04 at 25 fps (rate code 1).
  const tc = { h: 1, m: 2, s: 3, f: 4 };
  const vals = [tc.f & 15, tc.f >> 4, tc.s & 15, tc.s >> 4, tc.m & 15, tc.m >> 4, tc.h & 15, (tc.h >> 4) | (1 << 1)];
  let got = null;
  vals.forEach((v, i) => { got = p.message([0xf1, (i << 4) | v], 100 + i * 10) || got; });
  assert.equal(got.rate.id, '25');
  assert.equal(got.time, 170);
  assert.equal(formatTc(got), '01:02:03:04');
  assert.equal(got.frames, tcToFrames(tc, RATES['25']) + 2);
  // Missing a piece gives nothing.
  const q = new MtcParser();
  vals.forEach((v, i) => { if (i !== 3) got = q.message([0xf1, (i << 4) | v], 0); });
  assert.equal(got, null);
  // Full frame, 29.97 DF (rate code 2).
  const full = p.message([0xf0, 0x7f, 0x7f, 0x01, 0x01, (2 << 5) | 10, 20, 30, 15, 0xf7], 500);
  assert.equal(full.rate.id, '29.97DF');
  assert.equal(formatTc(full, true), '10:20:30;15');
  assert.equal(full.frames, tcToFrames({ h: 10, m: 20, s: 30, f: 15 }, RATES['29.97DF']));
});
