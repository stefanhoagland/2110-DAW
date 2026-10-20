// Built-in channel plugins for the live console.
// Each plugin type lists its parameters and builds a small Web Audio graph
// with an `input`, an `output` and `set(key, value)`.

const ms = (v) => v / 1000;
const dbGain = (db) => 10 ** (db / 20);
const smooth = (param, value, ctx) => param.setTargetAtTime(value, ctx.currentTime, 0.01);

function reverbImpulse(ctx, seconds) {
  const len = Math.max(1, Math.round(ctx.sampleRate * seconds));
  const buf = ctx.createBuffer(2, len, ctx.sampleRate);
  for (let c = 0; c < 2; c++) {
    const d = buf.getChannelData(c);
    for (let i = 0; i < len; i++) d[i] = (Math.random() * 2 - 1) * (1 - i / len) ** 3;
  }
  return buf;
}

// Dry/wet mixer used by time-based effects.
function wetDry(ctx) {
  const input = ctx.createGain();
  const output = ctx.createGain();
  const dry = ctx.createGain();
  const wet = ctx.createGain();
  input.connect(dry).connect(output);
  wet.connect(output);
  return {
    input, output, wet,
    mix(v) { smooth(dry.gain, Math.cos(v * Math.PI / 2), ctx); smooth(wet.gain, Math.sin(v * Math.PI / 2), ctx); },
  };
}

export const PLUGINS = {
  hpf: {
    name: 'High-pass',
    short: 'HPF',
    params: [
      { key: 'freq', label: 'Frequency', min: 20, max: 400, step: 1, def: 80, unit: 'Hz' },
    ],
    create(ctx) {
      const f = ctx.createBiquadFilter();
      f.type = 'highpass';
      f.Q.value = 0.707;
      return { input: f, output: f, set: (k, v) => smooth(f.frequency, v, ctx) };
    },
  },

  eq: {
    name: 'EQ',
    short: 'EQ',
    params: [
      { key: 'loGain', label: 'Low gain', min: -15, max: 15, step: 0.5, def: 0, unit: 'dB' },
      { key: 'loFreq', label: 'Low freq', min: 40, max: 400, step: 1, def: 120, unit: 'Hz' },
      { key: 'midGain', label: 'Mid gain', min: -15, max: 15, step: 0.5, def: 0, unit: 'dB' },
      { key: 'midFreq', label: 'Mid freq', min: 200, max: 6000, step: 10, def: 1000, unit: 'Hz' },
      { key: 'midQ', label: 'Mid width (Q)', min: 0.3, max: 6, step: 0.1, def: 0.9, unit: '' },
      { key: 'hiGain', label: 'High gain', min: -15, max: 15, step: 0.5, def: 0, unit: 'dB' },
      { key: 'hiFreq', label: 'High freq', min: 2000, max: 16000, step: 100, def: 8000, unit: 'Hz' },
    ],
    create(ctx) {
      const lo = ctx.createBiquadFilter(); lo.type = 'lowshelf';
      const mid = ctx.createBiquadFilter(); mid.type = 'peaking';
      const hi = ctx.createBiquadFilter(); hi.type = 'highshelf';
      lo.connect(mid).connect(hi);
      const map = {
        loGain: lo.gain, loFreq: lo.frequency, midGain: mid.gain, midFreq: mid.frequency,
        midQ: mid.Q, hiGain: hi.gain, hiFreq: hi.frequency,
      };
      return { input: lo, output: hi, set: (k, v) => smooth(map[k], v, ctx) };
    },
  },

  comp: {
    name: 'Compressor',
    short: 'COMP',
    params: [
      { key: 'threshold', label: 'Threshold', min: -60, max: 0, step: 0.5, def: -20, unit: 'dB' },
      { key: 'ratio', label: 'Ratio', min: 1, max: 20, step: 0.1, def: 4, unit: ':1' },
      { key: 'attack', label: 'Attack', min: 0, max: 100, step: 0.5, def: 5, unit: 'ms' },
      { key: 'release', label: 'Release', min: 10, max: 1000, step: 5, def: 150, unit: 'ms' },
      { key: 'knee', label: 'Knee', min: 0, max: 30, step: 1, def: 6, unit: 'dB' },
      { key: 'out', label: 'Output', min: -12, max: 12, step: 0.5, def: 0, unit: 'dB' },
    ],
    create(ctx) {
      const c = ctx.createDynamicsCompressor();
      const out = ctx.createGain();
      c.connect(out);
      return {
        input: c,
        output: out,
        meter: () => c.reduction,
        set(k, v) {
          if (k === 'out') smooth(out.gain, dbGain(v), ctx);
          else if (k === 'attack' || k === 'release') smooth(c[k], ms(v), ctx);
          else smooth(c[k], v, ctx);
        },
      };
    },
  },

  gate: {
    name: 'Gate',
    short: 'GATE',
    params: [
      { key: 'threshold', label: 'Threshold', min: -80, max: 0, step: 0.5, def: -50, unit: 'dB' },
      { key: 'range', label: 'Range', min: -80, max: 0, step: 1, def: -80, unit: 'dB' },
      { key: 'attack', label: 'Attack', min: 0.1, max: 50, step: 0.1, def: 2, unit: 'ms' },
      { key: 'hold', label: 'Hold', min: 0, max: 500, step: 5, def: 50, unit: 'ms' },
      { key: 'release', label: 'Release', min: 10, max: 1000, step: 5, def: 150, unit: 'ms' },
    ],
    create(ctx) {
      const g = new AudioWorkletNode(ctx, 'gate-processor');
      return { input: g, output: g, set: (k, v) => g.parameters.get(k).setValueAtTime(v, ctx.currentTime) };
    },
  },

  limiter: {
    name: 'Limiter',
    short: 'LIM',
    params: [
      { key: 'ceiling', label: 'Ceiling', min: -24, max: 0, step: 0.5, def: -1, unit: 'dB' },
      { key: 'drive', label: 'Input drive', min: 0, max: 24, step: 0.5, def: 0, unit: 'dB' },
    ],
    create(ctx) {
      const drive = ctx.createGain();
      const c = ctx.createDynamicsCompressor();
      c.knee.value = 0;
      c.ratio.value = 20;
      c.attack.value = 0.001;
      c.release.value = 0.08;
      drive.connect(c);
      return {
        input: drive,
        output: c,
        meter: () => c.reduction,
        set(k, v) {
          if (k === 'ceiling') smooth(c.threshold, v, ctx);
          else smooth(drive.gain, dbGain(v), ctx);
        },
      };
    },
  },

  delay: {
    name: 'Delay',
    short: 'DLY',
    params: [
      { key: 'time', label: 'Time', min: 10, max: 2000, step: 5, def: 350, unit: 'ms' },
      { key: 'feedback', label: 'Feedback', min: 0, max: 90, step: 1, def: 30, unit: '%' },
      { key: 'tone', label: 'Tone (low-pass)', min: 1000, max: 16000, step: 100, def: 6000, unit: 'Hz' },
      { key: 'mix', label: 'Mix', min: 0, max: 100, step: 1, def: 25, unit: '%' },
    ],
    create(ctx) {
      const wd = wetDry(ctx);
      const d = ctx.createDelay(2.5);
      const fb = ctx.createGain();
      const tone = ctx.createBiquadFilter();
      tone.type = 'lowpass';
      wd.input.connect(d).connect(tone).connect(wd.wet);
      tone.connect(fb).connect(d);
      return {
        input: wd.input,
        output: wd.output,
        set(k, v) {
          if (k === 'time') smooth(d.delayTime, ms(v), ctx);
          else if (k === 'feedback') smooth(fb.gain, v / 100, ctx);
          else if (k === 'tone') smooth(tone.frequency, v, ctx);
          else wd.mix(v / 100);
        },
      };
    },
  },

  reverb: {
    name: 'Reverb',
    short: 'VERB',
    params: [
      { key: 'decay', label: 'Decay', min: 0.3, max: 6, step: 0.1, def: 1.8, unit: 's' },
      { key: 'predelay', label: 'Pre-delay', min: 0, max: 150, step: 1, def: 20, unit: 'ms' },
      { key: 'tone', label: 'Tone (low-pass)', min: 1000, max: 16000, step: 100, def: 7000, unit: 'Hz' },
      { key: 'mix', label: 'Mix', min: 0, max: 100, step: 1, def: 20, unit: '%' },
    ],
    create(ctx) {
      const wd = wetDry(ctx);
      const pre = ctx.createDelay(0.5);
      const conv = ctx.createConvolver();
      const tone = ctx.createBiquadFilter();
      tone.type = 'lowpass';
      wd.input.connect(pre).connect(conv).connect(tone).connect(wd.wet);
      let timer = null;
      return {
        input: wd.input,
        output: wd.output,
        set(k, v) {
          if (k === 'decay') {
            clearTimeout(timer); // rebuilding the impulse is costly; wait for the slider to settle
            timer = setTimeout(() => { conv.buffer = reverbImpulse(ctx, v); }, conv.buffer ? 150 : 0);
          } else if (k === 'predelay') smooth(pre.delayTime, ms(v), ctx);
          else if (k === 'tone') smooth(tone.frequency, v, ctx);
          else wd.mix(v / 100);
        },
      };
    },
  },
};

export const PLUGIN_ORDER = ['hpf', 'gate', 'eq', 'comp', 'limiter', 'delay', 'reverb'];

export function pluginDefaults(type) {
  return Object.fromEntries(PLUGINS[type].params.map((p) => [p.key, p.def]));
}

export function formatParam(p, v) {
  const n = p.step >= 1 ? Math.round(v) : (p.step >= 0.1 ? v.toFixed(1) : v.toFixed(2));
  const sign = p.unit === 'dB' && v > 0 ? '+' : '';
  if (p.unit === 'Hz' && v >= 1000) return `${(v / 1000).toFixed(v >= 10000 ? 1 : 2)} kHz`;
  return `${sign}${n}${p.unit === ':1' ? ':1' : p.unit ? ` ${p.unit}` : ''}`;
}
