// Audio-thread processors used by plugins.

// Noise gate: opens when the level rises above the threshold, closes to
// `range` dB after the hold time once it falls below.
class GateProcessor extends AudioWorkletProcessor {
  static get parameterDescriptors() {
    return [
      { name: 'threshold', defaultValue: -50, automationRate: 'k-rate' },
      { name: 'attack', defaultValue: 2, automationRate: 'k-rate' }, // ms
      { name: 'hold', defaultValue: 50, automationRate: 'k-rate' }, // ms
      { name: 'release', defaultValue: 150, automationRate: 'k-rate' }, // ms
      { name: 'range', defaultValue: -80, automationRate: 'k-rate' }, // dB when closed
    ];
  }

  constructor() {
    super();
    this.env = 0;
    this.gain = 0;
    this.holdLeft = 0;
  }

  process(inputs, outputs, p) {
    const input = inputs[0];
    const output = outputs[0];
    if (!input.length) return true;
    const thr = 10 ** (p.threshold[0] / 20);
    const floor = 10 ** (p.range[0] / 20);
    const att = 1 - Math.exp(-1 / (sampleRate * Math.max(0.0001, p.attack[0] / 1000)));
    const rel = 1 - Math.exp(-1 / (sampleRate * Math.max(0.001, p.release[0] / 1000)));
    const envRel = 1 - Math.exp(-1 / (sampleRate * 0.01));
    const holdSamples = (p.hold[0] / 1000) * sampleRate;
    const n = input[0].length;
    for (let i = 0; i < n; i++) {
      let peak = 0;
      for (let c = 0; c < input.length; c++) {
        const v = Math.abs(input[c][i]);
        if (v > peak) peak = v;
      }
      this.env = peak > this.env ? peak : this.env + (peak - this.env) * envRel;
      let target;
      if (this.env >= thr) { target = 1; this.holdLeft = holdSamples; }
      else if (this.holdLeft > 0) { target = 1; this.holdLeft--; }
      else target = floor;
      this.gain += (target - this.gain) * (target > this.gain ? att : rel);
      for (let c = 0; c < output.length; c++) output[c][i] = (input[c] || input[0])[i] * this.gain;
    }
    return true;
  }
}

registerProcessor('gate-processor', GateProcessor);
