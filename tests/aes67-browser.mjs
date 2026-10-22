// Browser test of AES67 receive: a pretend sender on this machine announces two
// streams (SAP) and sends them (one unicast, one multicast on the loopback
// port). The page should list both, play them on the tracks picked, and name
// those tracks after the streams. Run with `node tests/aes67-browser.mjs`.
import dgram from 'node:dgram';
import { chromium } from 'playwright';
import { serve } from './serve.mjs';

const shot = process.env.SHOT; // optional screenshot path
// Set BASE_URL to test a running copy instead, e.g. the Docker container with host networking.
const base = process.env.BASE_URL || 'http://localhost:8112/';
const server = process.env.BASE_URL ? null : await serve(8112, { aes67: true });

const streams = [
  { name: 'Stage Box A', session: 7, origin: '127.0.0.1', addr: '127.0.0.1', port: 46300, pt: 98, ch: 8, level: 0.4 },
  { name: 'FOH Console Mix', session: 8, origin: '192.168.10.21', addr: '239.69.20.5', port: 5004, pt: 97, ch: 2, level: 0.3 },
];
const sdpOf = (s) => `v=0\r\no=- ${s.session} ${s.session} IN IP4 ${s.origin}\r\ns=${s.name}\r\nc=IN IP4 ${s.addr}/32\r\n` +
  `m=audio ${s.port} RTP/AVP ${s.pt}\r\na=rtpmap:${s.pt} L24/48000/${s.ch}\r\na=ptime:1\r\n`;

const sock = dgram.createSocket('udp4');
await new Promise((r) => sock.bind(r));
sock.setMulticastInterface('127.0.0.1');
sock.setMulticastLoopback(true);
let seq = 0;
const t0 = performance.now();
const tick = () => {
  if (seq % 1000 === 0) {
    for (const s of streams) {
      sock.send(Buffer.concat([Buffer.from([0x20, 0, 0x12, 0x34, 127, 0, 0, 1]), Buffer.from('application/sdp\0'), Buffer.from(sdpOf(s))]),
        9875, '239.255.255.255');
    }
  }
  while (seq < (performance.now() - t0)) { // one 1 ms packet per stream per millisecond
    for (const s of streams) {
      const b = Buffer.alloc(12 + 48 * s.ch * 3);
      b[0] = 0x80; b[1] = s.pt; b.writeUInt16BE(seq & 0xffff, 2); b.writeUInt32BE((seq * 48) >>> 0, 4); b.writeUInt32BE(s.session, 8);
      for (let f = 0; f < 48; f++) {
        for (let c = 0; c < s.ch; c++) {
          const v = Math.round(s.level * Math.sin((seq * 48 + f) * 0.03 * (c + 1)) * 0x7fffff);
          b.writeIntBE(v, 12 + (f * s.ch + c) * 3, 3);
        }
      }
      sock.send(b, s.port, s.addr);
    }
    seq++;
  }
};
const timer = setInterval(tick, 1);

const browser = await chromium.launch({
  args: ['--use-fake-ui-for-media-stream', '--use-fake-device-for-media-stream', '--autoplay-policy=no-user-gesture-required'],
});
let failed = false;
const check = (ok, what) => { console.log(`${ok ? 'ok ' : 'FAIL'} ${what}`); if (!ok) failed = true; };
try {
  const page = await browser.newPage({ viewport: { width: 1400, height: 900 }, ignoreHTTPSErrors: true });
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(base);
  await page.evaluate(() => localStorage.clear());
  await page.reload();
  await page.click('#btn-enable');
  await page.waitForFunction(() => window.daw.state.open, null, { timeout: 10000 });
  await page.click('#btn-aes67');
  await page.waitForFunction(() => window.daw.net.discovered.length === 2, null, { timeout: 5000 });
  check(true, 'both announced streams listed');
  await page.fill('#aes-search', 'foh');
  const found = await page.locator('.aes-stream').allTextContents();
  check(found.length === 1 && found[0].includes('FOH Console Mix'), 'search narrows the list');
  await page.fill('#aes-search', 'nothing like this');
  check((await page.textContent('#aes-list')).includes('No streams match'), 'search says when nothing matches');
  await page.focus('#aes-search');
  await page.keyboard.press('Escape');
  check(await page.locator('.aes-stream').count() === 2 && await page.locator('#aes67-dialog').evaluate((d) => d.open),
    'Esc clears the search and keeps the window open');
  await page.selectOption('#aes-iface', '127.0.0.1'); // the pretend sender is on the loopback port

  // Receive both; the 8-channel box on tracks 3-10, the console mix on 11-12.
  await page.locator('.aes-stream', { hasText: 'Stage Box A' }).locator('input[type=checkbox]').check();
  await page.waitForFunction(() => window.daw.net.subs.length === 1);
  await page.locator('.aes-stream', { hasText: 'Stage Box A' }).locator('select').selectOption('2');
  await page.locator('.aes-stream', { hasText: 'FOH Console Mix' }).locator('input[type=checkbox]').check();
  await page.waitForFunction(() => window.daw.net.subs.every((s) => s.server && s.server.receiving && s.local && s.local.playing), null, { timeout: 8000 })
    .catch(async (e) => { console.log(await page.evaluate(() => JSON.stringify(window.daw.net.subs.map((s) => ({ ...s, sdp: '' }))))); throw e; });
  check(true, 'both streams receiving and playing');
  await page.waitForTimeout(1500);
  const r = await page.evaluate(() => {
    const t = window.daw.tracks;
    return {
      names: t.slice(0, 14).map((x) => x.name),
      meters: t.slice(0, 14).map((x) => Math.round(x.meter * 100) / 100),
      subs: window.daw.net.subs.map((s) => ({ start: s.start, status: window.daw.net.status(s), local: s.local, server: s.server })),
      button: document.getElementById('btn-aes67').textContent,
    };
  });
  console.log(JSON.stringify(r, null, 1));
  check(r.names[2] === 'Stage Box A 1' && r.names[9] === 'Stage Box A 8', 'tracks 3-10 named after Stage Box A');
  const foh = r.subs.find((s) => s.start !== 2);
  check(foh && foh.start === 10 && r.names[10] === 'FOH Console Mix 1', 'console mix on the next free tracks, named');
  check(r.meters[2] > 0.3 && r.meters[5] > 0.3, 'Stage Box A level on its tracks');
  check(foh && r.meters[foh.start] > 0.2, 'multicast console mix level on its tracks');
  check(r.button === 'AES67 · 2 streams', 'button shows stream count');
  if (shot) await page.screenshot({ path: shot });

  // Untick the box: its tracks go back to their plain names.
  await page.locator('.aes-stream', { hasText: 'Stage Box A' }).locator('input[type=checkbox]').uncheck();
  await page.waitForFunction(() => window.daw.tracks[2].name === 'Ch 3', null, { timeout: 3000 });
  check(true, 'names go back when a stream is removed');
  check(errors.length === 0, `no page errors ${errors.join(' | ')}`);
} catch (err) {
  console.log('FAIL', err.message);
  failed = true;
} finally {
  clearInterval(timer);
  sock.close();
  await browser.close();
  if (server) { server.close(); server.aes67.close(); }
}
process.exit(failed ? 1 : 0);
