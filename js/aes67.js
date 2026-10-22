// AES67 / ST 2110-30 network audio for the web app. Browsers can't join network
// audio streams themselves, so the receiver runs in the Docker container
// (server/aes67-server.mjs) and passes audio here over a WebSocket. Each stream
// is patched to a run of tracks; its audio replaces the audio interface's on
// those tracks (js/aes67-worklet.js).

const STORE_KEY = 'daw2110.aes67';
export const LATENCY_CHOICES = [10, 20, 40, 80, 160];

export class NetworkAudio extends EventTarget {
  constructor(numTracks) {
    super();
    this.numTracks = numTracks;
    this.ws = null;
    this.available = false; // the container's receiver answered
    this.tried = false; // at least one connection attempt finished
    this.interfaces = [];
    this.iface = '';
    this.latencyMs = 20;
    this.discovered = [];
    this.subs = []; // [{ id, sdp, info, start, error, server: {}, local: {} }]
    this.nextId = 1;
    this.node = null;
    this.retry = null;
    this.load();
  }

  // ---------- settings ----------

  load() {
    try {
      const s = JSON.parse(localStorage.getItem(STORE_KEY) || '{}');
      if (typeof s.iface === 'string') this.iface = s.iface;
      if (LATENCY_CHOICES.includes(s.latencyMs)) this.latencyMs = s.latencyMs;
      for (const x of s.streams || []) {
        if (x && typeof x.sdp === 'string') this.subs.push(this.newSub(x.sdp, x.start | 0, x.info || null));
      }
    } catch (_) { /* storage unavailable */ }
  }

  save() {
    try {
      localStorage.setItem(STORE_KEY, JSON.stringify({
        iface: this.iface,
        latencyMs: this.latencyMs,
        streams: this.subs.map((s) => ({ sdp: s.sdp, start: s.start, info: s.info })),
      }));
    } catch (_) { /* storage unavailable */ }
  }

  newSub(sdp, start, info = null) {
    return { id: this.nextId++, sdp, info, start, error: '', server: null, local: null };
  }

  changed() {
    this.dispatchEvent(new Event('change'));
  }

  // ---------- connection to the container ----------

  connect() {
    if (this.ws) return;
    const url = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/aes67/ws`;
    let ws;
    try { ws = new WebSocket(url); } catch (_) { this.scheduleRetry(); return; }
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    ws.onmessage = (e) => this.onMessage(e.data);
    ws.onclose = () => {
      this.ws = null;
      this.available = false;
      this.tried = true;
      for (const s of this.subs) s.server = null;
      this.changed();
      this.scheduleRetry();
    };
  }

  scheduleRetry() {
    clearTimeout(this.retry);
    // Keep trying while a stream is wanted; otherwise the AES67 window connects when opened.
    if (this.subs.length) this.retry = setTimeout(() => this.connect(), 3000);
  }

  send(obj) {
    if (this.ws && this.ws.readyState === 1) this.ws.send(JSON.stringify(obj));
  }

  onMessage(data) {
    if (typeof data !== 'string') {
      const id = new DataView(data).getUint16(0);
      if (this.node) this.node.port.postMessage({ cmd: 'data', id, buf: data.slice(4) });
      return;
    }
    let msg;
    try { msg = JSON.parse(data); } catch (_) { return; }
    if (msg.type === 'hello') {
      this.available = true;
      this.tried = true;
      this.interfaces = msg.interfaces || [];
      if (!this.interfaces.some((i) => i.ip === this.iface)) this.iface = msg.iface || '';
      this.send({ cmd: 'iface', ip: this.iface });
      for (const s of this.subs) this.send({ cmd: 'subscribe', id: s.id, sdp: s.sdp });
    } else if (msg.type === 'streams') {
      this.discovered = msg.list || [];
    } else if (msg.type === 'subscribed') {
      const s = this.byId(msg.id);
      if (s) { s.info = msg.info; s.error = ''; this.configure(s); this.save(); }
    } else if (msg.type === 'error') {
      const s = this.byId(msg.id);
      if (s) s.error = msg.message;
    } else if (msg.type === 'stats') {
      for (const x of msg.list || []) { const s = this.byId(x.id); if (s) s.server = x; }
    }
    this.changed();
  }

  byId(id) {
    return this.subs.find((s) => s.id === id);
  }

  // ---------- audio thread ----------

  // Called when the audio engine (re)opens, with its network input node.
  attach(node) {
    this.node = node;
    if (!node) return;
    node.port.onmessage = (e) => {
      if (e.data.type !== 'netstats') return;
      for (const x of e.data.list) { const s = this.byId(x.id); if (s) s.local = x; }
      this.changed();
    };
    node.port.postMessage({ cmd: 'latency', ms: this.latencyMs });
    for (const s of this.subs) this.configure(s);
  }

  configure(s) {
    if (!this.node || !s.info) return;
    const { channels, bits, rate } = s.info;
    this.node.port.postMessage({ cmd: 'add', id: s.id, channels, bits, rate, start: s.start });
  }

  // ---------- what the page calls ----------

  setInterface(ip) {
    this.iface = ip;
    this.save();
    this.send({ cmd: 'iface', ip });
    for (const s of this.subs) this.send({ cmd: 'subscribe', id: s.id, sdp: s.sdp }); // join again on the new port
    this.changed();
  }

  setLatency(ms) {
    this.latencyMs = ms;
    this.save();
    if (this.node) this.node.port.postMessage({ cmd: 'latency', ms });
    this.changed();
  }

  subscription(key) {
    return this.subs.find((s) => s.info && s.info.key === key);
  }

  // The first track after `from` where `channels` tracks are free of other streams.
  freeStart(channels, from = 0) {
    const used = new Set();
    for (const s of this.subs) for (let c = 0; c < this.channelsOf(s); c++) used.add(s.start + c);
    const fits = (t) => { for (let c = 0; c < channels; c++) if (used.has(t + c)) return false; return true; };
    for (let t = from; t + channels <= this.numTracks; t++) if (fits(t)) return t;
    for (let t = 0; t + channels <= this.numTracks; t++) if (fits(t)) return t;
    return Math.max(0, Math.min(from, this.numTracks - 1));
  }

  channelsOf(s) {
    return s.info ? s.info.channels : 0;
  }

  // Starts receiving a stream given its SDP text and stream info, on tracks from `start` (0-based).
  add(sdp, info, start) {
    const s = this.newSub(sdp, start, info);
    this.subs.push(s);
    this.save();
    if (!this.ws) this.connect();
    this.send({ cmd: 'subscribe', id: s.id, sdp });
    this.changed();
    return s;
  }

  remove(id) {
    const i = this.subs.findIndex((s) => s.id === id);
    if (i < 0) return;
    this.subs.splice(i, 1);
    this.send({ cmd: 'unsubscribe', id });
    if (this.node) this.node.port.postMessage({ cmd: 'remove', id });
    this.save();
    this.changed();
  }

  move(id, start) {
    const s = this.byId(id);
    if (!s) return;
    s.start = start;
    this.configure(s);
    this.save();
    this.changed();
  }

  resetStats() {
    if (this.node) this.node.port.postMessage({ cmd: 'reset' });
  }

  // Which stream channel feeds a track: { sub, ch } or null.
  sourceOf(track) {
    for (const s of this.subs) {
      const ch = track - s.start;
      if (ch >= 0 && ch < this.channelsOf(s)) return { sub: s, ch };
    }
    return null;
  }

  // Name for a track fed by the network, like "Stage Box A 3", or ''.
  trackName(track) {
    const src = this.sourceOf(track);
    if (!src) return '';
    const n = src.sub.info.channels;
    return n === 1 ? src.sub.info.name : `${src.sub.info.name} ${src.ch + 1}`;
  }

  // Plain-text health summary, for troubleshooting.
  status(s) {
    if (s.error) return s.error;
    if (!this.available) return 'Waiting for the receiver';
    if (!s.server) return 'Joining…';
    if (!s.server.receiving) return 'No packets arriving';
    const end = Math.min(this.numTracks, s.start + this.channelsOf(s));
    const tracks = end - s.start > 1 ? `Tracks ${s.start + 1}-${end}` : `Track ${s.start + 1}`;
    const drops = s.local && s.local.dropouts ? ` · ${s.local.dropouts} dropout${s.local.dropouts === 1 ? '' : 's'}` : '';
    const lost = s.server.lost ? ` · ${s.server.lost} lost` : '';
    return `Receiving · ${tracks}${lost}${drops}`;
  }

  report(audioInfo) {
    const lines = [`Soundbending Live (web) AES67 report`, `Audio: ${audioInfo}`,
      `Receiver: ${this.available ? 'connected' : 'not reachable'} · network port ${this.iface || 'default'} · buffer ${this.latencyMs} ms`];
    for (const s of this.subs) {
      const i = s.info || {};
      lines.push('', `${i.name || 'Stream'} (${i.format || '?'}) ${i.address || ''}:${i.port || ''}${i.origin ? ` from ${i.origin}` : ''}`);
      lines.push(`  ${this.status(s)}`);
      if (s.server) lines.push(`  packets ${s.server.packets}, lost ${s.server.lost}, longest gap between packets ${s.server.maxGapMs} ms`);
      if (s.local) {
        lines.push(`  dropouts ${s.local.dropouts}, buffer in use ${Math.round(s.local.bufferMs)} ms, lowest left ` +
          `${s.local.lowestMs == null ? '-' : s.local.lowestMs.toFixed(1)} ms, clock correction ${s.local.driftPpm.toFixed(0)} ppm` +
          (s.local.overflows ? `, ${s.local.overflows} blocks dropped (page too slow)` : ''));
      }
    }
    return lines.join('\n');
  }
}
