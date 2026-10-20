// Runs on the audio thread. Splits the multichannel input into per-channel
// buffers, sends level meters to the UI, and while recording ships blocks of
// samples for the armed channels to the main thread.

const BLOCK_FRAMES = 4096; // ~85 ms at 48 kHz
const METER_FRAMES = 1024; // ~21 ms at 48 kHz

class RecorderProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.channels = options.processorOptions.channels;
    this.peaks = new Float32Array(this.channels);
    this.meterCount = 0;
    this.recording = false;
    this.armed = [];
    this.buffers = [];
    this.pos = 0;
    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  onMessage(msg) {
    if (msg.cmd === 'start') {
      this.armed = msg.armed; // channel indexes to capture
      this.buffers = this.armed.map(() => new Float32Array(BLOCK_FRAMES));
      this.pos = 0;
      this.recording = true;
      this.port.postMessage({ type: 'started', frame: currentFrame });
    } else if (msg.cmd === 'stop') {
      if (this.recording) this.flush();
      this.recording = false;
      this.port.postMessage({ type: 'stopped', frame: currentFrame });
    }
  }

  flush() {
    if (this.pos === 0) return;
    const data = this.buffers.map((b) => (this.pos === BLOCK_FRAMES ? b : b.slice(0, this.pos)));
    this.port.postMessage({ type: 'data', data }, data.map((d) => d.buffer));
    this.buffers = this.armed.map(() => new Float32Array(BLOCK_FRAMES));
    this.pos = 0;
  }

  process(inputs) {
    const input = inputs[0] || [];
    const frames = input[0] ? input[0].length : 128;

    for (let c = 0; c < this.channels; c++) {
      const ch = input[c];
      if (!ch) continue;
      let p = this.peaks[c];
      for (let i = 0; i < ch.length; i++) {
        const a = ch[i] < 0 ? -ch[i] : ch[i];
        if (a > p) p = a;
      }
      this.peaks[c] = p;
    }

    if (this.recording) {
      for (let k = 0; k < this.armed.length; k++) {
        const ch = input[this.armed[k]];
        if (ch) this.buffers[k].set(ch, this.pos);
        // Missing channels stay silent (buffers start zeroed).
      }
      this.pos += frames;
      if (this.pos >= BLOCK_FRAMES) this.flush();
    }

    this.meterCount += frames;
    if (this.meterCount >= METER_FRAMES) {
      this.port.postMessage({ type: 'meter', peaks: this.peaks.slice() });
      this.peaks.fill(0);
      this.meterCount = 0;
    }
    return true;
  }
}

registerProcessor('recorder-processor', RecorderProcessor);
