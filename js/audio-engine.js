// Audio input, recording and playback.

export const MAX_CHANNELS = 32; // Chrome allows at most 32 channels per input device

export class AudioEngine extends EventTarget {
  constructor() {
    super();
    this.ctx = null;
    this.stream = null;
    this.source = null;
    this.input = null; // all track inputs (interface + network)
    this.recorder = null;
    this.deviceChannels = 0;
    this.master = null;
    this.voices = [];
    this.playing = false;
    this.playStartCtxTime = 0;
    this.playStartOffset = 0;
    this.limiterOn = true;
    this.sinkId = '';
    this.ltcChannel = -1;
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
    await this.ctx.audioWorklet.addModule(new URL('./plugins-worklet.js', import.meta.url));
    await this.ctx.audioWorklet.addModule(new URL('./aes67-worklet.js', import.meta.url));

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
    // Every track's input: the interface's channels, with AES67 network streams
    // on the tracks they're patched to (see aes67.js).
    this.input = new AudioWorkletNode(this.ctx, 'aes67-input', {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [MAX_CHANNELS],
      channelCount: MAX_CHANNELS,
      channelCountMode: 'explicit',
      channelInterpretation: 'discrete',
      processorOptions: { channels: MAX_CHANNELS },
    });
    this.source.connect(this.input);
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
    this.recorder.port.postMessage({ cmd: 'ltc', channel: this.ltcChannel });
    this.input.connect(this.recorder);
    this.recorder.connect(this.ctx.destination); // outputs silence; keeps the node running

    // Master bus: master gain -> (optional limiter) -> output, metered after the limiter.
    this.master = this.ctx.createGain();
    this.limiter = this.ctx.createDynamicsCompressor();
    this.limiter.threshold.value = -1;
    this.limiter.knee.value = 0;
    this.limiter.ratio.value = 20;
    this.limiter.attack.value = 0.001;
    this.limiter.release.value = 0.1;
    this.out = this.ctx.createGain();
    this.out.connect(this.ctx.destination);
    this.limiter.connect(this.out);
    this.setLimiter(this.limiterOn);
    // Taps for the master meter (left and right of the output).
    const split = this.ctx.createChannelSplitter(2);
    this.out.connect(split);
    this.masterTaps = [0, 1].map((ch) => {
      const a = this.ctx.createAnalyser();
      a.fftSize = 2048;
      split.connect(a, ch);
      return a;
    });
    this.tapBuf = new Float32Array(2048);
    if (this.sinkId) await this.setOutput(this.sinkId).catch(() => {});
    await this.ctx.resume();
    this.dispatchEvent(new Event('opened'));
    return { channels: this.deviceChannels, sampleRate: this.ctx.sampleRate, label: this.stream.getAudioTracks()[0].label };
  }

  async close() {
    this.stopPlayback();
    if (this.stream) this.stream.getTracks().forEach((t) => t.stop());
    if (this.ctx) await this.ctx.close();
    this.ctx = this.stream = this.source = this.input = this.recorder = this.master = this.masterTaps = null;
    this.limiter = this.out = null;
  }

  // Brick-wall-style limiter on the master output, to keep a stream from clipping.
  setLimiter(on) {
    this.limiterOn = !!on;
    if (!this.master) return;
    this.master.disconnect();
    this.master.connect(this.limiterOn ? this.limiter : this.out);
  }

  async listOutputs() {
    const devices = await navigator.mediaDevices.enumerateDevices();
    return devices.filter((d) => d.kind === 'audiooutput');
  }

  get canChooseOutput() {
    return typeof AudioContext !== 'undefined' && 'setSinkId' in AudioContext.prototype;
  }

  // Send the whole mix to another output device (e.g. a virtual cable OBS listens to).
  async setOutput(deviceId) {
    this.sinkId = deviceId || '';
    if (this.ctx && this.canChooseOutput) await this.ctx.setSinkId(this.sinkId);
  }

  // Rough input-to-output delay of the browser audio path, in seconds.
  get latency() {
    if (!this.ctx) return 0;
    const track = this.stream && this.stream.getAudioTracks()[0];
    const inLat = (track && track.getSettings().latency) || 0;
    return inLat + (this.ctx.baseLatency || 0) + (this.ctx.outputLatency || 0);
  }

  // Decode LTC timecode from this input channel (0-based), or -1 for none.
  // Decoded frames arrive as 'ltc' events: { frames: [{ h, m, s, f, df, pos }] }.
  setLtcChannel(channel) {
    this.ltcChannel = channel;
    if (this.recorder) this.recorder.port.postMessage({ cmd: 'ltc', channel });
  }

  // The current audio input position, in samples, in the count the recorder
  // worklet uses (the 'started' frame and LTC positions), for a moment given
  // in performance.now() milliseconds (default: now). Approximate (~ms).
  inputFrameAt(perfMs = performance.now()) {
    if (!this.ctx) return 0;
    const sr = this.ctx.sampleRate;
    const ts = this.ctx.getOutputTimestamp ? this.ctx.getOutputTimestamp() : null;
    if (ts && ts.performanceTime > 0 && ts.contextTime > 0) {
      return (ts.contextTime + (perfMs - ts.performanceTime) / 1000 + this.latency) * sr;
    }
    const track = this.stream && this.stream.getAudioTracks()[0];
    const inLat = (track && track.getSettings().latency) || 0;
    return (this.ctx.currentTime + inLat + (perfMs - performance.now()) / 1000) * sr;
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
