import test from 'node:test';
import assert from 'node:assert/strict';
import dgram from 'node:dgram';
import { parseSdp, parseSap, parseRtp, streamKey, formatStream } from '../server/aes67.mjs';
import { parseSdpText } from '../js/aes67-sdp.js';
import { Aes67Service, startServer } from '../server/aes67-server.mjs';

const SDP = [
  'v=0', 'o=- 1311738121 1311738121 IN IP4 192.168.1.20', 's=Stage Box A', 'c=IN IP4 239.69.1.10/32',
  't=0 0', 'm=audio 5004 RTP/AVP 98', 'c=IN IP4 239.69.1.11/32', 'a=rtpmap:98 L24/48000/8', 'a=ptime:0.125',
  'a=source-filter: incl IN IP4 239.69.1.11 192.168.1.21', 'a=ts-refclk:ptp=IEEE1588-2008:00-00-00-00-00-00-00-00:0',
].join('\r\n');

test('SDP: reads address, format and sender', () => {
  const r = parseSdp(SDP);
  assert.ok(r.ok, r.why);
  const s = r.info;
  assert.equal(s.name, 'Stage Box A');
  assert.equal(s.address, '239.69.1.11'); // the media c= line wins
  assert.equal(s.port, 5004);
  assert.equal(s.payloadType, 98);
  assert.equal(s.channels, 8);
  assert.equal(s.bits, 24);
  assert.equal(s.rate, 48000);
  assert.equal(s.ptimeMs, 0.125);
  assert.equal(s.sourceFilter, '192.168.1.21');
  assert.equal(s.origin, '192.168.1.20');
  assert.equal(formatStream(s), '8 ch · L24 · 48 kHz · 125 µs');
  assert.equal(streamKey(s), '192.168.1.20/1311738121/239.69.1.11:5004');
  assert.equal(parseSdpText(SDP).info.format, '8 ch · L24 · 48 kHz · 125 µs');
});

test('SDP: refuses what it cannot play, with a reason', () => {
  assert.match(parseSdp('v=0\ns=x\nc=IN IP4 239.1.1.1\nm=audio 5004 RTP/AVP 97\na=rtpmap:97 opus/48000/2').why, /OPUS isn't supported/);
  assert.match(parseSdp('v=0\ns=x\nm=audio 5004 RTP/AVP 97\na=rtpmap:97 L24/48000/2').why, /no address/);
  assert.match(parseSdp('v=0\ns=x\nc=IN IP4 239.1.1.1\nm=video 5004 RTP/AVP 96').why, /No audio/);
  const l16 = parseSdp('v=0\ns=-\nc=IN IP4 239.1.1.1\nm=audio 5006 RTP/AVP 97\na=rtpmap:97 L16/44100/2\na=ptime:1');
  assert.ok(l16.ok);
  assert.equal(l16.info.name, '239.1.1.1'); // no session name: show the address
  assert.equal(formatStream(l16.info), '2 ch · L16 · 44.1 kHz · 1 ms');
});

const sapPacket = (sdp, { deleted = false, type = true } = {}) => Buffer.concat([
  Buffer.from([0x20 | (deleted ? 0x04 : 0), 0, 0x12, 0x34, 192, 168, 1, 20]),
  type ? Buffer.from('application/sdp\0') : Buffer.alloc(0),
  Buffer.from(sdp),
]);

test('SAP: finds the SDP with or without a payload type, and goodbyes', () => {
  assert.equal(parseSap(sapPacket(SDP)).sdp, SDP);
  assert.equal(parseSap(sapPacket(SDP, { type: false })).sdp, SDP);
  assert.equal(parseSap(sapPacket(SDP, { deleted: true })).deleted, true);
  assert.equal(parseSap(Buffer.from([0x20, 0, 0, 0, 1, 2, 3, 4])), null);
  assert.equal(parseSap(Buffer.from([0x23, 0, 0, 0, 1, 2, 3, 4, 0x76])), null); // encrypted
});

function rtp(seq, payload, pt = 98) {
  const h = Buffer.alloc(12);
  h[0] = 0x80; h[1] = pt; h.writeUInt16BE(seq & 0xffff, 2); h.writeUInt32BE(seq * 6, 4); h.writeUInt32BE(1, 8);
  return Buffer.concat([h, payload]);
}

test('RTP: finds the payload past CSRCs, extensions and padding', () => {
  const p = rtp(7, Buffer.from([1, 2, 3, 4, 5, 6]));
  assert.deepEqual(parseRtp(p), { seq: 7, timestamp: 42, payloadType: 98, start: 12, end: 18 });
  const ext = Buffer.concat([p.subarray(0, 12), Buffer.from([0xbe, 0xde, 0, 1, 9, 9, 9, 9]), p.subarray(12), Buffer.from([0, 0, 3])]);
  ext[0] = 0x80 | 0x10 | 0x20;
  const r = parseRtp(ext);
  assert.equal(r.start, 20);
  assert.equal(r.end, ext.length - 3);
  assert.equal(parseRtp(Buffer.alloc(8)), null);
});

test('service: SAP announcements fill the list and goodbyes clear it', async () => {
  const svc = new Aes67Service({ sap: false });
  try {
    svc.onSap(sapPacket(SDP));
    assert.equal(svc.streamList().length, 1);
    assert.equal(svc.streamList()[0].name, 'Stage Box A');
    assert.equal(svc.streamList()[0].format, '8 ch · L24 · 48 kHz · 125 µs');
    svc.onSap(sapPacket(SDP, { deleted: true }));
    assert.equal(svc.streamList().length, 0);
  } finally {
    svc.close();
  }
});

// A 2-channel L24 stream sent to this machine; the browser side is a WebSocket.
test('service: passes a stream to a browser, filling lost packets with silence', async () => {
  const port = 46200 + Math.floor(Math.random() * 500);
  const { server, service } = await startServer(0, '127.0.0.1');
  const ws = new WebSocket(`ws://127.0.0.1:${server.address().port}/aes67/ws`);
  ws.binaryType = 'arraybuffer';
  const got = { hello: null, subscribed: null, error: null, stats: null, bytes: 0, samples: [] };
  ws.onmessage = (e) => {
    if (typeof e.data === 'string') {
      const m = JSON.parse(e.data);
      got[m.type] = m;
      return;
    }
    const v = new DataView(e.data);
    assert.equal(v.getUint16(0), 5);
    for (let p = 4; p + 3 <= e.data.byteLength; p += 3) {
      got.samples.push(((v.getUint8(p) << 24) | (v.getUint8(p + 1) << 16) | (v.getUint8(p + 2) << 8)) / 2 ** 31);
    }
    got.bytes += e.data.byteLength - 4;
  };
  const until = async (fn, ms = 3000) => {
    const end = Date.now() + ms;
    while (!fn()) {
      if (Date.now() > end) throw new Error('timed out');
      await new Promise((r) => setTimeout(r, 10));
    }
  };
  const sender = dgram.createSocket('udp4');
  try {
    await until(() => got.hello);
    assert.ok(Array.isArray(got.hello.interfaces));
    ws.send(JSON.stringify({ cmd: 'iface', ip: '127.0.0.1' }));
    const sdp = `v=0\r\no=- 9 9 IN IP4 127.0.0.1\r\ns=Test Tone\r\nc=IN IP4 127.0.0.1\r\nm=audio ${port} RTP/AVP 98\r\na=rtpmap:98 L24/48000/2\r\na=ptime:1\r\n`;
    ws.send(JSON.stringify({ cmd: 'subscribe', id: 5, sdp }));
    await until(() => got.subscribed || got.error);
    assert.equal(got.error, null);
    assert.equal(got.subscribed.info.name, 'Test Tone');

    // 1 ms packets of a constant 0.5 on the left, -0.25 on the right; packet 3 goes missing.
    const frame = Buffer.from([0x40, 0, 0, 0xe0, 0, 0]);
    const payload = Buffer.concat(Array(48).fill(frame));
    for (let seq = 0; seq < 10; seq++) {
      if (seq === 3) continue;
      sender.send(rtp(seq, payload), port, '127.0.0.1');
      await new Promise((r) => setTimeout(r, 1));
    }
    await until(() => got.bytes >= 10 * payload.length);
    assert.equal(got.bytes, 10 * payload.length); // 9 packets plus one of silence
    const zeros = got.samples.filter((x) => x === 0).length;
    assert.equal(zeros, 96); // the missing packet: 48 frames x 2 channels
    assert.equal(got.samples[0], 0.5);
    assert.equal(got.samples[1], -0.25);
    await until(() => got.stats);
    assert.equal(got.stats.list[0].id, 5);
    assert.equal(got.stats.list[0].packets, 9);
    assert.equal(got.stats.list[0].lost, 1);

    ws.send(JSON.stringify({ cmd: 'unsubscribe', id: 5 }));
    await until(() => service.receivers.size === 0);
    ws.send(JSON.stringify({ cmd: 'subscribe', id: 6, sdp: 'v=0\r\ns=x\r\n' }));
    await until(() => got.error);
    assert.match(got.error.message, /No audio/);
  } finally {
    sender.close();
    ws.close();
    service.close();
    server.close();
  }
});

// The audio-thread side, run outside a browser.
test('worklet: plays a stream on its tracks and follows a faster sender clock', async () => {
  let Proc;
  globalThis.sampleRate = 48000;
  globalThis.AudioWorkletProcessor = class { constructor() { this.port = { postMessage: (m) => posted.push(m) }; } };
  globalThis.registerProcessor = (_, c) => { Proc = c; };
  const posted = [];
  await import('../js/aes67-worklet.js');
  const p = new Proc({ processorOptions: { channels: 32 } });
  p.onMessage({ cmd: 'latency', ms: 10 });
  p.onMessage({ cmd: 'add', id: 1, channels: 2, bits: 16, rate: 48000, start: 4 });

  // The sender runs 100 ppm fast; the receiver should read slightly faster to keep up.
  const senderRate = 48000 * 1.0001;
  let sent = 0, due = 0, phase = 0;
  const feed = (ms) => {
    due += senderRate * ms / 1000;
    const frames = Math.floor(due - sent);
    const b = new Uint8Array(frames * 4);
    for (let f = 0; f < frames; f++, phase++) {
      const v = Math.round(Math.sin(phase * 0.05) * 16000);
      b[f * 4] = (v >> 8) & 255; b[f * 4 + 1] = v & 255; // left: tone
      b[f * 4 + 2] = 0x20; b[f * 4 + 3] = 0; // right: 0.25
    }
    sent += frames;
    p.onMessage({ cmd: 'data', id: 1, buf: b.buffer });
  };
  const out = Array.from({ length: 32 }, () => new Float32Array(128));
  const dev = Array.from({ length: 2 }, () => new Float32Array(128).fill(0.9));
  let peak4 = 0, right = 0, passthrough = 0;
  for (let block = 0; block < 48000 * 20 / 128; block++) { // 20 seconds
    if (block % 3 === 0) feed(128 * 3 / 48); // ~8 ms of audio every 3 blocks
    p.process([dev], [out]);
    if (block > 1000) {
      for (const x of out[4]) peak4 = Math.max(peak4, Math.abs(x));
      right = out[5][64];
      passthrough = out[0][10];
    }
  }
  const s = p.streams.get(1);
  assert.ok(peak4 > 0.45 && peak4 < 0.5, `tone peak ${peak4}`);
  assert.ok(Math.abs(right - 0.25) < 1e-6, `right ${right}`);
  assert.ok(Math.abs(passthrough - 0.9) < 1e-6, 'interface channels pass through');
  assert.equal(s.dropouts, 0);
  assert.ok(s.drift > 50 && s.drift < 150, `clock correction ${s.drift} ppm`);
  const leftMs = 1000 * (s.written - s.pos) / 48000;
  assert.ok(leftMs > 5 && leftMs < 40, `buffer ${leftMs} ms`);

  // Starve it: a dropout, and the buffer grows by itself.
  for (let block = 0; block < 400; block++) p.process([dev], [out]);
  assert.ok(s.dropouts >= 1);
  assert.equal(s.extraMs, 5);
  assert.ok(posted.some((m) => m.type === 'netstats'));
});
