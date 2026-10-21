// External timecode for the clock and for stamping recordings: LTC decoded
// from an audio input channel (in the recorder worklet), or MIDI timecode from
// a MIDI input. Keeps an anchor (frame count at a sample position) and
// interpolates from it, freewheeling when the signal stops.

import { framesToTc, formatTc, rateText, framesPerDay, timeReference, tcToFrames, LtcRateEstimator, MtcParser } from './timecode.js';

const STORE_KEY = 'daw2110.timecode';
const LOST_MS = 500; // no frames for this long = no signal

export class TimecodeSource extends EventTarget {
  constructor(engine) {
    super();
    this.engine = engine;
    this.setting = { type: 'off', channel: 0, midi: '', midiName: '' };
    this.midi = null; // MIDIAccess, once asked for
    this.midiError = '';
    this.input = null; // the MIDI input we listen to
    this.reset();
    engine.addEventListener('ltc', (e) => this.onLtc(e.detail.frames));
    engine.addEventListener('opened', () => { this.reset(); this.apply(); });
  }

  reset() {
    this.anchor = null; // { frames, pos }: frame count since midnight at sample position pos
    this.rate = null;
    this.lastSeen = -Infinity; // performance.now() of the last frame received
    this.lastLabel = null; // newest LTC label while the rate is still unknown
    this.shown = null;
    this.estimator = null;
    this.mtc = new MtcParser();
  }

  load() {
    try {
      const s = JSON.parse(localStorage.getItem(STORE_KEY) || 'null');
      if (s && ['off', 'ltc', 'mtc'].includes(s.type)) {
        this.setting = {
          type: s.type,
          channel: Number.isInteger(s.channel) ? s.channel : 0,
          midi: typeof s.midi === 'string' ? s.midi : '',
          midiName: typeof s.midiName === 'string' ? s.midiName : '',
        };
      }
    } catch (_) { /* storage unavailable */ }
  }

  save() {
    try { localStorage.setItem(STORE_KEY, JSON.stringify(this.setting)); } catch (_) { /* storage unavailable */ }
  }

  get active() { return this.setting.type !== 'off'; }
  get kind() { return this.setting.type === 'mtc' ? 'MTC' : 'LTC'; }
  get receiving() { return performance.now() - this.lastSeen < LOST_MS; }
  get locked() { return this.active && this.receiving && !!this.anchor && !!this.rate; }

  async set(setting) {
    this.setting = { ...this.setting, ...setting };
    this.save();
    this.reset();
    await this.apply();
  }

  async apply() {
    this.engine.setLtcChannel(this.setting.type === 'ltc' ? this.setting.channel : -1);
    if (this.input) { this.input.onmidimessage = null; this.input = null; }
    if (this.setting.type !== 'mtc') return;
    try {
      await this.requestMidi();
    } catch (_) {
      return;
    }
    this.attachMidi();
  }

  attachMidi() {
    if (this.input) { this.input.onmidimessage = null; this.input = null; }
    if (!this.midi || this.setting.type !== 'mtc') return;
    const list = [...this.midi.inputs.values()];
    const s = this.setting;
    const input = list.find((i) => i.id === s.midi) || list.find((i) => s.midiName && i.name === s.midiName);
    if (!input) return;
    this.input = input;
    input.onmidimessage = (e) => this.handleMidi(e.data, e.timeStamp);
  }

  // Ask for MIDI access, with sysex (needed for full-frame MTC) if allowed.
  async requestMidi() {
    if (this.midi) return this.midi;
    if (!navigator.requestMIDIAccess) {
      this.midiError = 'MIDI not available in this browser';
      throw new Error(this.midiError);
    }
    try {
      this.midi = await navigator.requestMIDIAccess({ sysex: true });
    } catch (_) {
      try {
        this.midi = await navigator.requestMIDIAccess({ sysex: false });
      } catch (err) {
        this.midiError = 'MIDI access was not allowed';
        throw err;
      }
    }
    this.midiError = '';
    this.midi.onstatechange = () => {
      if (!this.input || this.input.state === 'disconnected') this.attachMidi();
      this.dispatchEvent(new Event('devices'));
    };
    return this.midi;
  }

  midiInputs() {
    return this.midi ? [...this.midi.inputs.values()].map((i) => ({ id: i.id, name: i.name || 'MIDI input' })) : [];
  }

  // Sample position (in the recorder worklet's count) for a performance.now() time.
  posAt(perfMs) {
    return this.engine.ctx ? this.engine.inputFrameAt(perfMs) : perfMs * 48;
  }

  get sampleRate() {
    return this.engine.sampleRate || 48000;
  }

  onLtc(frames) {
    if (this.setting.type !== 'ltc' || !frames.length) return;
    const sr = this.sampleRate;
    if (!this.estimator || this.estimator.sampleRate !== sr) this.estimator = new LtcRateEstimator(sr);
    let rate = null;
    for (const f of frames) rate = this.estimator.push(f);
    const f = frames[frames.length - 1];
    this.lastSeen = performance.now();
    this.lastLabel = f;
    if (!rate) return;
    this.rate = rate;
    // The sync word ends where the next frame starts.
    this.anchor = { frames: tcToFrames(f, rate) + 1, pos: f.pos };
  }

  // MIDI bytes and their timestamp (performance.now() ms).
  handleMidi(data, timeStamp) {
    if (this.setting.type !== 'mtc') return;
    const t = timeStamp > 0 ? timeStamp : performance.now();
    if (data[0] === 0xf1 || data[0] === 0xf0) this.lastSeen = performance.now();
    const a = this.mtc.message(data, t);
    if (!a) return;
    this.rate = a.rate;
    this.anchor = { frames: a.frames, pos: this.posAt(t) };
  }

  // Frame count since midnight (fractional) at a sample position.
  framesAt(pos) {
    if (!this.anchor || !this.rate) return null;
    const day = framesPerDay(this.rate);
    const n = this.anchor.frames + ((pos - this.anchor.pos) / this.sampleRate) * this.rate.fps;
    return ((n % day) + day) % day;
  }

  // Clock text for now: HH:MM:SS:FF (';' before frames for drop-frame).
  text() {
    if (!this.anchor || !this.rate) {
      return this.lastLabel ? formatTc(this.lastLabel, this.lastLabel.df) : '--:--:--:--';
    }
    let n = this.framesAt(this.posAt(performance.now()));
    // Don't step back a frame because of timing jitter between updates.
    if (this.shown !== null && n < this.shown && this.shown - n < 2) n = this.shown;
    this.shown = n;
    return formatTc(framesToTc(n, this.rate), this.rate.df);
  }

  status() {
    if (this.setting.type === 'mtc') {
      if (this.midiError) return `MTC · ${this.midiError}`;
      if (this.midi && !this.input) return 'MTC · MIDI input not connected';
    } else if (!this.engine.ctx) {
      return 'LTC · enable audio';
    }
    if (!this.receiving) return `${this.kind} · no signal`;
    if (!this.rate) return `${this.kind} · locking…`;
    return `${this.kind} ${rateText(this.rate)}`;
  }

  // Timecode of the sample at `pos` for a Broadcast Wave stamp.
  stampAt(pos) {
    const n = this.framesAt(pos);
    if (n === null) return null;
    return {
      label: formatTc(framesToTc(n, this.rate), this.rate.df),
      rate: this.rate,
      timeReference: timeReference(n, this.rate, this.sampleRate),
    };
  }
}
