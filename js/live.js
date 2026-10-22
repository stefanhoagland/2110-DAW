// Live mode: a mixing console for the live inputs, for streaming.
// Each input channel runs through gain -> its plugin inserts (in order,
// skipping bypassed ones) -> fader -> pan into a live bus that feeds the
// master output.

import { PLUGINS, pluginDefaults } from './plugins.js';
import { createWamInsert, nameFromUrl } from './wam.js';

const STORE_KEY = 'daw2110.live';
const FADER_MIN_DB = -60;
const FADER_MAX_DB = 10;

const dbToGain = (db) => (db <= FADER_MIN_DB ? 0 : 10 ** (db / 20));
const faderToDb = (v) => (v <= 0 ? -Infinity : FADER_MIN_DB + v * (FADER_MAX_DB - FADER_MIN_DB));
const dbToFader = (db) => (db === -Infinity ? 0 : (db - FADER_MIN_DB) / (FADER_MAX_DB - FADER_MIN_DB));
const fmtDb = (db) => (db === -Infinity ? '-∞' : `${db > 0.05 ? '+' : ''}${Math.abs(db) < 0.05 ? '0.0' : db.toFixed(1)}`);

const defaults = () => ({
  trim: 0, fader: dbToFader(0), pan: 0, mute: false, solo: false, plugins: [],
});

let nextId = 1;
const newPlugin = (type, values) => ({ id: nextId++, type, bypass: false, values: { ...pluginDefaults(type), ...values } });
// A third-party Web Audio Module; `state` is the plugin's own saved settings.
const newWam = (url, name, state = null) => ({
  id: nextId++, type: 'wam', bypass: false, url, name: name || nameFromUrl(url), state, values: {},
});
const isWam = (x) => x && x.type === 'wam' && typeof x.url === 'string';

export function pluginName(pl) {
  return isWam(pl) ? pl.name : PLUGINS[pl.type].name;
}

// Settings saved before plugins existed had fixed HPF/EQ/compressor controls.
function migrate(p) {
  const plugins = Array.isArray(p.plugins) ? p.plugins.filter((x) => isWam(x) || (x && PLUGINS[x.type])) : [];
  if (p.hpf) plugins.push(newPlugin('hpf'));
  if (p.lo || p.mid || p.hi) plugins.push(newPlugin('eq', { loGain: p.lo || 0, midGain: p.mid || 0, hiGain: p.hi || 0 }));
  if (p.comp) plugins.push(newPlugin('comp', { threshold: -40 * p.comp, ratio: 1 + 7 * p.comp }));
  for (const k of ['hpf', 'lo', 'mid', 'hi', 'comp']) delete p[k];
  p.plugins = plugins.map((x) => ({
    ...(isWam(x) ? newWam(x.url, x.name, x.state) : newPlugin(x.type, x.values)),
    bypass: !!x.bypass,
  }));
  return p;
}

export class LiveMixer {
  constructor(engine, tracks, { onChange, onPluginReady } = {}) {
    this.engine = engine;
    this.tracks = tracks;
    this.onChange = onChange || (() => {});
    this.onPluginReady = onPluginReady || (() => {});
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
      (saved.strips || []).forEach((p, i) => { if (this.params[i] && p) Object.assign(this.params[i], migrate({ ...p })); });
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
    const { ctx, input, master } = this.engine;
    if (!ctx || !input) return;
    this.chains.forEach((c) => c.inserts.forEach((inst) => inst.dispose && inst.dispose()));
    const n = this.tracks.length;
    const split = ctx.createChannelSplitter(n);
    input.connect(split);
    this.bus = ctx.createGain();
    this.bus.gain.value = this.on ? 1 : 0;
    this.bus.connect(master);
    this.chains = this.tracks.map((_, i) => {
      const trim = ctx.createGain();
      const fader = ctx.createGain();
      const pan = ctx.createStereoPanner();
      const meter = ctx.createAnalyser();
      meter.fftSize = 512;
      split.connect(trim, i);
      fader.connect(pan).connect(this.bus);
      fader.connect(meter);
      const chain = { trim, fader, pan, meter, inserts: [] };
      chain.inserts = this.params[i].plugins.map((pl) => this.instantiate(pl, i));
      return chain;
    });
    this.chains.forEach((_, i) => this.rewire(i));
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

  // ---------- plugin inserts ----------

  instantiate(pl, i) {
    if (pl.type === 'wam') {
      return createWamInsert(this.engine.ctx, pl, () => { this.save(); this.onPluginReady(i, pl); });
    }
    const inst = PLUGINS[pl.type].create(this.engine.ctx);
    for (const [k, v] of Object.entries(pl.values)) inst.set(k, v);
    return inst;
  }

  // Connect trim -> active inserts -> fader for one channel.
  rewire(i) {
    const c = this.chains[i];
    if (!c) return;
    c.trim.disconnect();
    c.inserts.forEach((inst) => inst.output.disconnect());
    let node = c.trim;
    this.params[i].plugins.forEach((pl, k) => {
      if (pl.bypass) return;
      node.connect(c.inserts[k].input);
      node = c.inserts[k].output;
    });
    node.connect(c.fader);
  }

  addPlugin(i, type, url) {
    const pl = type === 'wam' ? newWam(url) : newPlugin(type);
    this.params[i].plugins.push(pl);
    if (this.chains[i]) this.chains[i].inserts.push(this.instantiate(pl, i));
    this.rewire(i);
    this.save();
    return this.params[i].plugins.length - 1;
  }

  removePlugin(i, k) {
    this.params[i].plugins.splice(k, 1);
    const c = this.chains[i];
    if (c) {
      const [inst] = c.inserts.splice(k, 1);
      if (inst.dispose) inst.dispose(); else inst.output.disconnect();
    }
    this.rewire(i);
    this.save();
  }

  movePlugin(i, k, dir) {
    const to = k + dir;
    const list = this.params[i].plugins;
    if (to < 0 || to >= list.length) return k;
    [list[k], list[to]] = [list[to], list[k]];
    const c = this.chains[i];
    if (c) [c.inserts[k], c.inserts[to]] = [c.inserts[to], c.inserts[k]];
    this.rewire(i);
    this.save();
    return to;
  }

  setBypass(i, k, bypass) {
    this.params[i].plugins[k].bypass = bypass;
    this.rewire(i);
    this.save();
  }

  setPluginParam(i, k, key, value) {
    this.params[i].plugins[k].values[key] = value;
    const c = this.chains[i];
    if (c) c.inserts[k].set(key, value);
    this.save();
  }

  // The loaded insert (for a WAM: { wam, error, ready }).
  insert(i, k) {
    return (this.chains[i] && this.chains[i].inserts[k]) || null;
  }

  // Ask every loaded WAM for its current settings and save them; their
  // editors change settings without telling us.
  async saveWamStates() {
    let changed = false;
    await Promise.all(this.chains.flatMap((c, i) => c.inserts.map(async (inst, k) => {
      const pl = this.params[i].plugins[k];
      if (!inst.wam || !pl || pl.type !== 'wam') return;
      try {
        const st = await inst.wam.audioNode.getState();
        if (JSON.stringify(st) !== JSON.stringify(pl.state)) { pl.state = st; changed = true; }
      } catch (_) { /* plugin without state support */ }
    })));
    if (changed) this.save();
  }

  // Gain reduction (dB, 0 or below) of a dynamics plugin, if it has one.
  pluginMeter(i, k) {
    const inst = this.chains[i] && this.chains[i].inserts[k];
    return inst && inst.meter ? inst.meter() : null;
  }

}

export { faderToDb, dbToFader, fmtDb, defaults };
