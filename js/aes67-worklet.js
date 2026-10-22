// Runs on the audio thread. Passes the audio interface's channels through and
// puts AES67 network streams on the tracks they're patched to. Network audio
// arrives in blocks from the page (which gets it from the container's receiver
// over a WebSocket). Each stream keeps a small buffer and follows the sender's
// clock by reading a touch faster or slower, so PTP isn't needed. Same method
// as the Mac app.

const RING_SECONDS = 1;
const MAX_EXTRA_MS = 200; // the most a stream's buffer grows by itself after dropouts
const EXTRA_STEP_MS = 5;

class Stream {
  constructor({ id, channels, bits, rate, start }) {
    this.id = id;
    this.channels = channels;
    this.bits = bits;
    this.rate = rate;
    this.start = start; // first track (0-based)
    this.cap = Math.ceil(rate * RING_SECONDS);
    this.ring = new Float32Array(this.cap * channels);
    this.written = 0; // frames written, ever
    this.pos = 0; // read position in frames (fractional), ever
    this.buffering = true;
    this.fill = 0;
    this.extraMs = 0;
    this.dropouts = 0;
    this.overflows = 0;
    this.lowestMs = Infinity;
    this.drift = 0;
    this.bufferMs = 0;
  }

  // Big-endian L16/L24 interleaved samples.
  push(bytes) {
    const ch = this.channels;
    const bps = this.bits / 8;
    const frames = Math.floor(bytes.length / (bps * ch));
    if (frames <= 0) return;
    if (this.written + frames - Math.floor(this.pos) > this.cap - 2) { this.overflows++; return; } // reader stalled; drop
    let p = 0;
    let w = (this.written % this.cap) * ch;
    const ring = this.ring;
    const size = ring.length;
    for (let k = 0, n = frames * ch; k < n; k++, p += bps) {
      const v = bps === 3
        ? ((bytes[p] << 24) | (bytes[p + 1] << 16) | (bytes[p + 2] << 8))
        : ((bytes[p] << 24) | (bytes[p + 1] << 16));
      ring[w] = v / 2147483648;
      if (++w === size) w = 0;
    }
    this.written += frames;
  }

  // Writes `n` samples at the context rate into the output channels it's patched to.
  read(out, n, ctxRate, latencyMs) {
    const ch = this.channels;
    const base = this.rate / ctxRate; // stream samples per output sample
    const bufferMs = latencyMs + this.extraMs;
    this.bufferMs = bufferMs;
    const target = bufferMs * 0.001 * this.rate + n * base;
    let avail = this.written - this.pos;
    if (this.buffering) {
      if (avail < target) return;
      this.buffering = false;
      this.fill = avail;
    }
    // Too far behind (e.g. after the tab was busy): jump to the target rather than lag.
    if (avail > target + 0.1 * this.rate) {
      this.pos = this.written - target;
      this.fill = target;
      avail = target;
    }
    this.fill += 0.02 * (avail - this.fill);
    // Read a touch faster when the buffer grows, slower when it shrinks: 0.2% at most.
    const corr = Math.max(-0.002, Math.min(0.002, (this.fill - target) / (this.rate * 0.5)));
    const ratio = base * (1 + corr);
    const need = Math.ceil(n * ratio) + 2;
    if (avail < need) {
      this.dropouts++;
      this.extraMs = Math.min(MAX_EXTRA_MS, this.extraMs + EXTRA_STEP_MS);
      this.buffering = true;
      return;
    }
    const ring = this.ring;
    const cap = this.cap;
    for (let c = 0; c < ch; c++) {
      const dest = out[this.start + c];
      if (!dest) continue;
      let pos = this.pos;
      for (let i = 0; i < n; i++, pos += ratio) {
        const f0 = Math.floor(pos);
        const frac = pos - f0;
        const a = ring[(f0 % cap) * ch + c];
        const b = ring[((f0 + 1) % cap) * ch + c];
        dest[i] = a + (b - a) * frac;
      }
    }
    this.pos += n * ratio;
    const leftMs = 1000 * (this.written - this.pos) / this.rate;
    if (leftMs < this.lowestMs) this.lowestMs = leftMs;
    this.drift += 0.001 * (corr * 1e6 - this.drift);
  }
}

class Aes67Processor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.channels = options.processorOptions.channels;
    this.latencyMs = 20;
    this.streams = new Map();
    this.statsCount = 0;
    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  onMessage(msg) {
    if (msg.cmd === 'data') {
      const s = this.streams.get(msg.id);
      if (s) s.push(new Uint8Array(msg.buf));
    } else if (msg.cmd === 'add') {
      this.streams.set(msg.id, new Stream(msg));
    } else if (msg.cmd === 'remove') {
      this.streams.delete(msg.id);
    } else if (msg.cmd === 'latency') {
      this.latencyMs = msg.ms;
    } else if (msg.cmd === 'reset') {
      for (const s of this.streams.values()) s.lowestMs = Infinity;
    }
  }

  process(inputs, outputs) {
    const input = inputs[0] || [];
    const out = outputs[0];
    const n = out[0] ? out[0].length : 128;
    for (let c = 0; c < out.length; c++) {
      if (input[c]) out[c].set(input[c]);
      else out[c].fill(0);
    }
    for (const s of this.streams.values()) {
      // Patched tracks carry only the network audio.
      for (let c = 0; c < s.channels; c++) if (out[s.start + c]) out[s.start + c].fill(0);
      s.read(out, n, sampleRate, this.latencyMs);
    }
    this.statsCount += n;
    if (this.statsCount >= sampleRate / 2 && this.streams.size) {
      this.statsCount = 0;
      this.port.postMessage({
        type: 'netstats',
        list: [...this.streams.values()].map((s) => ({
          id: s.id,
          bufferMs: s.bufferMs,
          dropouts: s.dropouts,
          overflows: s.overflows,
          lowestMs: Number.isFinite(s.lowestMs) ? s.lowestMs : null,
          driftPpm: s.drift,
          playing: !s.buffering,
        })),
      });
    }
    return true;
  }
}

registerProcessor('aes67-input', Aes67Processor);
