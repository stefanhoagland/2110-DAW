// Live mode: a mixing console for the live inputs, for streaming.
// Each input channel runs through gain -> high-pass -> 3-band EQ ->
// compressor -> fader -> pan into a live bus that feeds the master output.

const STORE_KEY = 'daw2110.live';
const FADER_MIN_DB = -60;
const FADER_MAX_DB = 10;

const dbToGain = (db) => (db <= FADER_MIN_DB ? 0 : 10 ** (db / 20));
const faderToDb = (v) => (v <= 0 ? -Infinity : FADER_MIN_DB + v * (FADER_MAX_DB - FADER_MIN_DB));
const dbToFader = (db) => (db === -Infinity ? 0 : (db - FADER_MIN_DB) / (FADER_MAX_DB - FADER_MIN_DB));
const fmtDb = (db) => (db === -Infinity ? '-∞' : `${db > 0.05 ? '+' : ''}${Math.abs(db) < 0.05 ? '0.0' : db.toFixed(1)}`);

const defaults = () => ({
  trim: 0, hpf: false, lo: 0, mid: 0, hi: 0, comp: 0, fader: dbToFader(0), pan: 0, mute: false, solo: false,
});

export class LiveMixer {
  constructor(engine, tracks, { onChange } = {}) {
    this.engine = engine;
    this.tracks = tracks;
    this.onChange = onChange || (() => {});
    this.params = tracks.map(() => defaults());
    this.on = false; // monitoring/output of the live mix
    this.chains = [];
    this.bus = null;
    this.load();
    engine.addEventListener('opened', () => this.build());
  }

  load() {
    try {
      const saved = JSON.parse(localStorage.getItem(STORE_KEY) || '{}');
      (saved.strips || []).forEach((p, i) => { if (this.params[i] && p) Object.assign(this.params[i], p); });
      if (typeof saved.limiter === 'boolean') this.engine.limiterOn = saved.limiter;
      if (typeof saved.output === 'string') this.engine.sinkId = saved.output;
    } catch (_) { /* storage unavailable */ }
  }

  save() {
    try {
      localStorage.setItem(STORE_KEY, JSON.stringify({
        strips: this.params, limiter: this.engine.limiterOn, output: this.engine.sinkId,
      }));
    } catch (_) { /* storage unavailable */ }
  }

  // (Re)create the audio graph after the input device opens.
  build() {
    const { ctx, source, master } = this.engine;
    if (!ctx || !source) return;
    const n = this.tracks.length;
    const split = ctx.createChannelSplitter(n);
    source.connect(split);
    this.bus = ctx.createGain();
    this.bus.gain.value = this.on ? 1 : 0;
    this.bus.connect(master);
    this.chains = this.tracks.map((_, i) => {
      const trim = ctx.createGain();
      const hpf = ctx.createBiquadFilter();
      hpf.type = 'highpass';
      const lo = ctx.createBiquadFilter();
      lo.type = 'lowshelf';
      lo.frequency.value = 120;
      const mid = ctx.createBiquadFilter();
      mid.type = 'peaking';
      mid.frequency.value = 1000;
      mid.Q.value = 0.9;
      const hi = ctx.createBiquadFilter();
      hi.type = 'highshelf';
      hi.frequency.value = 8000;
      const comp = ctx.createDynamicsCompressor();
      comp.knee.value = 6;
      comp.attack.value = 0.005;
      comp.release.value = 0.15;
      const fader = ctx.createGain();
      const pan = ctx.createStereoPanner();
      const meter = ctx.createAnalyser();
      meter.fftSize = 512;
      split.connect(trim, i);
      trim.connect(hpf).connect(lo).connect(mid).connect(hi).connect(comp).connect(fader).connect(pan).connect(this.bus);
      fader.connect(meter);
      return { trim, hpf, lo, mid, hi, comp, fader, pan, meter };
    });
    this.meterBuf = new Float32Array(512);
    this.params.forEach((_, i) => this.apply(i));
  }

  setOn(on) {
    this.on = on;
    if (this.bus) this.bus.gain.setTargetAtTime(on ? 1 : 0, this.engine.ctx.currentTime, 0.02);
  }

  set(i, key, value) {
    this.params[i][key] = value;
    if (key === 'solo') this.params.forEach((_, k) => this.apply(k));
    else this.apply(i);
    this.save();
  }

  apply(i) {
    const c = this.chains[i];
    const p = this.params[i];
    if (!c) return;
    const t = this.engine.ctx.currentTime;
    const anySolo = this.params.some((x) => x.solo);
    const audible = !p.mute && (!anySolo || p.solo);
    c.trim.gain.setTargetAtTime(dbToGain(p.trim), t, 0.01);
    c.hpf.frequency.setTargetAtTime(p.hpf ? 80 : 10, t, 0.01);
    c.lo.gain.setTargetAtTime(p.lo, t, 0.01);
    c.mid.gain.setTargetAtTime(p.mid, t, 0.01);
    c.hi.gain.setTargetAtTime(p.hi, t, 0.01);
    c.comp.threshold.setTargetAtTime(-40 * p.comp, t, 0.01);
    c.comp.ratio.setTargetAtTime(1 + 7 * p.comp, t, 0.01);
    c.fader.gain.setTargetAtTime(audible ? dbToGain(faderToDb(p.fader)) : 0, t, 0.01);
    c.pan.pan.setTargetAtTime(p.pan, t, 0.01);
  }

  // Post-fader peak of a channel (0..1+).
  peak(i) {
    const c = this.chains[i];
    if (!c) return 0;
    c.meter.getFloatTimeDomainData(this.meterBuf);
    let m = 0;
    for (let k = 0; k < this.meterBuf.length; k++) {
      const v = this.meterBuf[k] < 0 ? -this.meterBuf[k] : this.meterBuf[k];
      if (v > m) m = v;
    }
    return m;
  }

  // Gain reduction of a channel's compressor, in dB (0 or negative).
  reduction(i) {
    const c = this.chains[i];
    return c ? c.comp.reduction : 0;
  }

  reset(i) {
    this.params[i] = defaults();
    this.apply(i);
    this.save();
  }
}

export { faderToDb, dbToFader, fmtDb, defaults };
