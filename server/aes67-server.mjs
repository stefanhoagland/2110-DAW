// AES67 / ST 2110-30 receiver for the web app. Browsers can't join multicast
// network audio, so this small service runs next to the web server (inside the
// Docker container), listens for SAP announcements, joins the streams a browser
// asks for and passes their audio to it over a WebSocket at /aes67/ws.
//
// No dependencies: Node's own dgram, http and crypto.
//
// Messages, browser -> service (JSON text):
//   { cmd: 'iface', ip }               network port to join streams on ('' = first)
//   { cmd: 'subscribe', id, sdp }      start receiving a stream; id is the browser's number for it
//   { cmd: 'unsubscribe', id }
// Service -> browser:
//   { type: 'hello', interfaces: [{ name, ip }], iface }
//   { type: 'streams', list: [stream info] }   SAP discoveries, newest first
//   { type: 'subscribed', id, info } / { type: 'error', id, message }
//   { type: 'stats', list: [{ id, packets, lost, receiving, maxGapMs }] }  every second
//   binary: [u16 id][u16 0][big-endian L16/L24 interleaved samples]       a few ms of audio

import dgram from 'node:dgram';
import http from 'node:http';
import os from 'node:os';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { parseSdp, parseSap, parseRtp, streamKey, formatStream } from './aes67.mjs';

const SAP_GROUP = '239.255.255.255';
const SAP_PORT = 9875;
const SAP_EXPIRE_MS = 5 * 60 * 1000;
const SEND_MS = 2; // audio goes to the browser in blocks of about this length
const MAX_QUEUED = 1 << 20; // bytes waiting on a slow browser before audio is skipped

const isMulticast = (ip) => { const a = parseInt(ip, 10); return a >= 224 && a <= 239; };

export function localInterfaces() {
  const list = [], loop = [];
  for (const [name, addrs] of Object.entries(os.networkInterfaces())) {
    for (const a of addrs || []) {
      if (a.family !== 'IPv4' && a.family !== 4) continue;
      (a.internal ? loop : list).push({ name, ip: a.address });
    }
  }
  return [...list, ...loop];
}

// Public part of a stream description sent to browsers.
const publicInfo = (s) => ({ ...s, key: streamKey(s), format: formatStream(s) });

//==============================================================================
// One multicast (or unicast) stream on one network port, shared by every browser
// listening to it.
class Receiver {
  constructor(info, iface, onEmpty) {
    this.info = info;
    this.iface = iface;
    this.onEmpty = onEmpty;
    this.listeners = new Map(); // client -> id
    this.packets = 0;
    this.lost = 0;
    this.lastPacketMs = 0;
    this.lastArrival = 0;
    this.maxGapMs = 0;
    this.haveSeq = false;
    this.lastSeq = 0;
    this.frameBytes = (info.bits / 8) * info.channels;
    this.sendBytes = Math.max(1, Math.round(info.rate * SEND_MS / 1000)) * this.frameBytes;
    this.pending = [];
    this.pendingBytes = 0;
  }

  open() {
    const { address, port } = this.info;
    return new Promise((resolve) => {
      const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true });
      this.sock = sock;
      let settled = false;
      const fail = (msg) => { if (!settled) { settled = true; this.close(); resolve(msg); } };
      sock.on('error', (err) => fail(err.code === 'EADDRINUSE' ? `Port ${port} is busy.` : err.message));
      sock.on('message', (msg, rinfo) => this.onPacket(msg, rinfo));
      // Binding to the group means this socket only gets that group's packets,
      // even when several streams share port 5004.
      sock.bind({ port, address: isMulticast(address) ? address : undefined }, () => {
        try {
          if (isMulticast(address)) sock.addMembership(address, this.iface || undefined);
        } catch (err) {
          fail(`Can't join ${address} on that network port (${err.code || err.message}).`);
          return;
        }
        try { sock.setRecvBufferSize(4 << 20); } catch (_) { /* capped by the system */ }
        settled = true;
        resolve('');
      });
    });
  }

  close() {
    if (this.sock) { try { this.sock.close(); } catch (_) { /* already closed */ } }
    this.sock = null;
  }

  onPacket(buf, rinfo) {
    const s = this.info;
    if (s.sourceFilter && rinfo.address !== s.sourceFilter) return;
    const r = parseRtp(buf);
    if (!r || r.payloadType !== s.payloadType) return;
    const bytes = r.end - r.start - ((r.end - r.start) % this.frameBytes);
    if (bytes <= 0) return;
    if (this.haveSeq) {
      const gap = (r.seq - this.lastSeq) & 0xffff;
      if (gap === 0 || gap > 32768) return; // duplicate or late
      if (gap > 1) {
        this.lost += gap - 1;
        if (gap - 1 <= 20) this.queue(Buffer.alloc((gap - 1) * bytes)); // keep time with silence
      }
    }
    this.haveSeq = true;
    this.lastSeq = r.seq;
    this.queue(buf.subarray(r.start, r.start + bytes));
    const now = performance.now();
    if (this.lastArrival && now - this.lastArrival > this.maxGapMs) this.maxGapMs = now - this.lastArrival;
    this.lastArrival = now;
    this.packets++;
    this.lastPacketMs = Date.now();
  }

  queue(chunk) {
    this.pending.push(chunk);
    this.pendingBytes += chunk.length;
    if (this.pendingBytes < this.sendBytes) return;
    const payload = Buffer.concat(this.pending, this.pendingBytes);
    this.pending = [];
    this.pendingBytes = 0;
    for (const [client, id] of this.listeners) client.sendAudio(id, payload);
  }

  stats() {
    const out = {
      packets: this.packets,
      lost: this.lost,
      receiving: Date.now() - this.lastPacketMs < 1000,
      maxGapMs: Math.round(this.maxGapMs * 10) / 10,
    };
    return out;
  }
}

//==============================================================================
// A tiny WebSocket connection (RFC 6455): text and binary out, text in.
class Client {
  constructor(service, socket) {
    this.service = service;
    this.socket = socket;
    this.buf = Buffer.alloc(0);
    this.subs = new Map(); // id -> Receiver
    this.iface = '';
    this.skipped = 0;
    socket.setNoDelay(true);
    socket.on('data', (d) => this.onData(d));
    socket.on('close', () => this.service.drop(this));
    socket.on('error', () => socket.destroy());
  }

  frame(opcode, payload) {
    const n = payload.length;
    let head;
    if (n < 126) head = Buffer.from([0x80 | opcode, n]);
    else if (n < 65536) { head = Buffer.alloc(4); head[0] = 0x80 | opcode; head[1] = 126; head.writeUInt16BE(n, 2); }
    else { head = Buffer.alloc(10); head[0] = 0x80 | opcode; head[1] = 127; head.writeBigUInt64BE(BigInt(n), 2); }
    if (this.socket.destroyed) return;
    this.socket.cork();
    this.socket.write(head);
    this.socket.write(payload);
    this.socket.uncork();
  }

  send(obj) { this.frame(1, Buffer.from(JSON.stringify(obj))); }

  sendAudio(id, payload) {
    if (this.socket.writableLength > MAX_QUEUED) { this.skipped++; return; } // browser can't keep up
    const head = Buffer.alloc(4);
    head.writeUInt16BE(id, 0);
    this.frame(2, Buffer.concat([head, payload]));
  }

  onData(d) {
    this.buf = this.buf.length ? Buffer.concat([this.buf, d]) : d;
    for (;;) {
      const b = this.buf;
      if (b.length < 2) return;
      const opcode = b[0] & 15;
      const masked = (b[1] & 0x80) !== 0;
      let len = b[1] & 127, at = 2;
      if (len === 126) { if (b.length < 4) return; len = b.readUInt16BE(2); at = 4; }
      else if (len === 127) { if (b.length < 10) return; len = Number(b.readBigUInt64BE(2)); at = 10; }
      if (len > 1 << 20) { this.socket.destroy(); return; }
      const maskAt = at;
      if (masked) at += 4;
      if (b.length < at + len) return;
      const payload = Buffer.from(b.subarray(at, at + len));
      if (masked) for (let i = 0; i < len; i++) payload[i] ^= b[maskAt + (i & 3)];
      this.buf = b.subarray(at + len);
      if (opcode === 8) { this.frame(8, Buffer.alloc(0)); this.socket.end(); return; }
      if (opcode === 9) this.frame(10, payload);
      else if (opcode === 1) {
        let msg = null;
        try { msg = JSON.parse(payload.toString('utf8')); } catch (_) { /* ignore */ }
        if (msg) this.service.command(this, msg).catch((err) => this.send({ type: 'error', id: msg.id, message: err.message }));
      }
    }
  }
}

//==============================================================================
export class Aes67Service {
  constructor({ sap = true } = {}) {
    this.clients = new Set();
    this.receivers = new Map(); // key@iface -> Receiver
    this.discovered = new Map(); // stream key -> { info, lastMs }
    this.sapSockets = [];
    if (sap) this.startSap();
    this.statsTimer = setInterval(() => this.sendStats(), 1000);
    this.gapTimer = setInterval(() => { for (const r of this.receivers.values()) r.maxGapMs = 0; }, 10000);
  }

  close() {
    clearInterval(this.statsTimer);
    clearInterval(this.gapTimer);
    clearTimeout(this.listTimer);
    for (const s of this.sapSockets) { try { s.close(); } catch (_) { /* closed */ } }
    for (const r of this.receivers.values()) r.close();
    for (const c of this.clients) c.socket.destroy();
  }

  // ---------- discovery ----------

  startSap() {
    const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true });
    sock.on('error', (err) => console.error(`AES67: can't listen for SAP announcements: ${err.message}`));
    sock.on('message', (msg) => this.onSap(msg));
    sock.bind({ port: SAP_PORT, address: SAP_GROUP }, () => {
      // Hear announcements on every network port.
      for (const i of localInterfaces()) {
        try { sock.addMembership(SAP_GROUP, i.ip); } catch (_) { /* that port has no multicast */ }
      }
    });
    this.sapSockets.push(sock);
    this.sweepTimer = setInterval(() => {
      const now = Date.now();
      let changed = false;
      for (const [k, d] of this.discovered) if (now - d.lastMs > SAP_EXPIRE_MS) { this.discovered.delete(k); changed = true; }
      if (changed) this.listChanged();
    }, 10000);
    this.sapSockets.push({ close: () => clearInterval(this.sweepTimer) });
  }

  onSap(buf) {
    const sap = parseSap(buf);
    if (!sap) return;
    const r = parseSdp(sap.sdp);
    if (!r.ok) return;
    const key = streamKey(r.info);
    if (sap.deleted) {
      if (this.discovered.delete(key)) this.listChanged();
      return;
    }
    const old = this.discovered.get(key);
    this.discovered.set(key, { info: r.info, lastMs: Date.now() });
    if (!old || old.info.sdp !== r.info.sdp) this.listChanged();
  }

  streamList() {
    return [...this.discovered.values()].sort((a, b) => b.lastMs - a.lastMs).map((d) => publicInfo(d.info));
  }

  listChanged() {
    clearTimeout(this.listTimer);
    this.listTimer = setTimeout(() => {
      const msg = { type: 'streams', list: this.streamList() };
      for (const c of this.clients) c.send(msg);
    }, 200);
  }

  // ---------- browsers ----------

  // Takes over an HTTP upgrade request for /aes67/ws.
  handleUpgrade(req, socket) {
    const key = req.headers['sec-websocket-key'];
    if (!key || (req.headers.upgrade || '').toLowerCase() !== 'websocket') { socket.destroy(); return; }
    const accept = createHash('sha1').update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`).digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
    const c = new Client(this, socket);
    this.clients.add(c);
    const ifs = localInterfaces();
    c.send({ type: 'hello', interfaces: ifs, iface: (ifs[0] && ifs[0].ip) || '' });
    c.send({ type: 'streams', list: this.streamList() });
  }

  drop(c) {
    if (!this.clients.delete(c)) return;
    for (const r of c.subs.values()) this.release(r, c);
    c.subs.clear();
  }

  release(r, c) {
    r.listeners.delete(c);
    if (r.listeners.size) return;
    r.close();
    for (const [k, v] of this.receivers) if (v === r) this.receivers.delete(k);
  }

  async command(c, msg) {
    if (msg.cmd === 'iface') {
      c.iface = typeof msg.ip === 'string' ? msg.ip : '';
    } else if (msg.cmd === 'subscribe') {
      const id = msg.id | 0;
      if (id < 1 || id > 65535) throw new Error('Bad stream number.');
      const p = parseSdp(msg.sdp);
      if (!p.ok) { c.send({ type: 'error', id, message: p.why }); return; }
      if (c.subs.has(id)) this.release(c.subs.get(id), c);
      c.subs.delete(id);
      const iface = c.iface || ((localInterfaces()[0] || {}).ip) || '';
      const rkey = `${streamKey(p.info)}@${iface}`;
      let r = this.receivers.get(rkey);
      if (!r) {
        r = new Receiver(p.info, iface);
        this.receivers.set(rkey, r);
        const err = await r.open();
        if (err) {
          this.receivers.delete(rkey);
          c.send({ type: 'error', id, message: err });
          return;
        }
      }
      if (!this.clients.has(c)) { if (!r.listeners.size) this.release(r, c); return; } // left meanwhile
      r.listeners.set(c, id);
      c.subs.set(id, r);
      c.send({ type: 'subscribed', id, info: publicInfo(p.info) });
    } else if (msg.cmd === 'unsubscribe') {
      const r = c.subs.get(msg.id | 0);
      if (r) { c.subs.delete(msg.id | 0); this.release(r, c); }
    }
  }

  sendStats() {
    for (const c of this.clients) {
      if (!c.subs.size) continue;
      c.send({ type: 'stats', list: [...c.subs].map(([id, r]) => ({ id, ...r.stats() })), skipped: c.skipped });
    }
  }
}

// Serves /aes67/ws (and /aes67/health) on its own port; nginx passes those paths here.
export function startServer(port = 8067, host = '127.0.0.1') {
  const service = new Aes67Service();
  const server = http.createServer((req, res) => {
    if (req.url.startsWith('/aes67/health')) {
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ ok: true, interfaces: localInterfaces(), streams: service.discovered.size }));
      return;
    }
    res.writeHead(404).end();
  });
  server.on('upgrade', (req, socket) => {
    if (req.url.startsWith('/aes67/ws')) service.handleUpgrade(req, socket);
    else socket.destroy();
  });
  return new Promise((resolve) => server.listen(port, host, () => resolve({ server, service })));
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const port = Number(process.env.AES67_PORT) || 8067;
  await startServer(port, process.env.AES67_HOST || '127.0.0.1');
  const ifs = localInterfaces().map((i) => `${i.name} ${i.ip}`).join(', ');
  console.log(`AES67 receiver ready on port ${port} (network ports: ${ifs || 'none'})`);
}
