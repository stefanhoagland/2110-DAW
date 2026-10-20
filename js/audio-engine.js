// Audio input, recording and playback.

export const MAX_CHANNELS = 16;

export class AudioEngine extends EventTarget {
  constructor() {
    super();
    this.ctx = null;
    this.stream = null;
    this.source = null;
    this.recorder = null;
    this.deviceChannels = 0;
    this.master = null;
    this.voices = [];
    this.playing = false;
    this.playStartCtxTime = 0;
    this.playStartOffset = 0;
  }

  get sampleRate() {
    return this.ctx ? this.ctx.sampleRate : 0;
  }

  // Ask for mic permission once so device names become visible.
  async unlockDeviceLabels() {
    const s = await navigator.mediaDevices.getUserMedia({ audio: true });
    s.getTracks().forEach((t) => t.stop());
  }

  async listInputs() {
    const devices = await navigator.mediaDevices.enumerateDevices();
    return devices.filter((d) => d.kind === 'audioinput');
  }

  async open({ deviceId, sampleRate }) {
    await this.close();
    const opts = { latencyHint: 'interactive' };
    if (sampleRate) opts.sampleRate = sampleRate;
    this.ctx = new AudioContext(opts);
    await this.ctx.audioWorklet.addModule(new URL('./recorder-worklet.js', import.meta.url));

    this.stream = await navigator.mediaDevices.getUserMedia({
      audio: {
        deviceId: deviceId ? { exact: deviceId } : undefined,
        channelCount: { ideal: MAX_CHANNELS },
        sampleRate: { ideal: this.ctx.sampleRate },
        echoCancellation: false,
        noiseSuppression: false,
        autoGainControl: false,
      },
    });
    const settings = this.stream.getAudioTracks()[0].getSettings();
    this.deviceChannels = settings.channelCount || 2;

    this.source = this.ctx.createMediaStreamSource(this.stream);
    this.recorder = new AudioWorkletNode(this.ctx, 'recorder-processor', {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [1],
      channelCount: MAX_CHANNELS,
      channelCountMode: 'explicit',
      channelInterpretation: 'discrete',
      processorOptions: { channels: MAX_CHANNELS },
    });
    this.recorder.port.onmessage = (e) => this.dispatchEvent(new CustomEvent(e.data.type, { detail: e.data }));
    this.source.connect(this.recorder);
    this.recorder.connect(this.ctx.destination); // outputs silence; keeps the node running

    this.master = this.ctx.createGain();
    this.master.connect(this.ctx.destination);
    // Taps for the master meter (left and right of the playback mix).
    const split = this.ctx.createChannelSplitter(2);
    this.master.connect(split);
    this.masterTaps = [0, 1].map((ch) => {
      const a = this.ctx.createAnalyser();
      a.fftSize = 2048;
      split.connect(a, ch);
      return a;
    });
    this.tapBuf = new Float32Array(2048);
    await this.ctx.resume();
    return { channels: this.deviceChannels, sampleRate: this.ctx.sampleRate, label: this.stream.getAudioTracks()[0].label };
  }

  async close() {
    this.stopPlayback();
    if (this.stream) this.stream.getTracks().forEach((t) => t.stop());
    if (this.ctx) await this.ctx.close();
    this.ctx = this.stream = this.source = this.recorder = this.master = this.masterTaps = null;
  }

  startRecording(armed) {
    this.recorder.port.postMessage({ cmd: 'start', armed });
  }

  stopRecording() {
    return new Promise((resolve) => {
      const done = () => { this.removeEventListener('stopped', done); resolve(); };
      this.addEventListener('stopped', done);
      this.recorder.port.postMessage({ cmd: 'stop' });
    });
  }

  // tracks: [{ buffer, gain, pan }] where gain already includes mute/solo.
  play(tracks, offset) {
    this.stopPlayback();
    const when = this.ctx.currentTime + 0.05;
    this.voices = tracks.map((t) => {
      const gain = this.ctx.createGain();
      gain.gain.value = t.gain;
      const pan = this.ctx.createStereoPanner();
      pan.pan.value = t.pan;
      gain.connect(pan).connect(this.master);
      let src = null;
      if (t.buffer && offset < t.buffer.duration) {
        src = this.ctx.createBufferSource();
        src.buffer = t.buffer;
        src.connect(gain);
        src.start(when, offset);
      }
      return { src, gain, pan };
    });
    this.playing = true;
    this.playStartCtxTime = when;
    this.playStartOffset = offset;
  }

  setVoice(index, { gain, pan }) {
    const v = this.voices[index];
    if (!v) return;
    const t = this.ctx.currentTime;
    if (gain !== undefined) v.gain.gain.setTargetAtTime(gain, t, 0.01);
    if (pan !== undefined) v.pan.pan.setTargetAtTime(pan, t, 0.01);
  }

  stopPlayback() {
    for (const v of this.voices) {
      try { v.src && v.src.stop(); } catch (_) { /* already stopped */ }
      v.gain.disconnect();
    }
    this.voices = [];
    this.playing = false;
  }

  get playPosition() {
    if (!this.playing) return this.playStartOffset;
    return this.playStartOffset + Math.max(0, this.ctx.currentTime - this.playStartCtxTime);
  }

  // Peak level of the last ~40 ms of the master output, [left, right].
  masterPeaks() {
    if (!this.masterTaps) return [0, 0];
    return this.masterTaps.map((a) => {
      a.getFloatTimeDomainData(this.tapBuf);
      let m = 0;
      for (let i = 0; i < this.tapBuf.length; i++) {
        const v = this.tapBuf[i] < 0 ? -this.tapBuf[i] : this.tapBuf[i];
        if (v > m) m = v;
      }
      return m;
    });
  }

  decode(arrayBuffer) {
    return this.ctx.decodeAudioData(arrayBuffer);
  }
}
