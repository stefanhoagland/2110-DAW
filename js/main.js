import { AudioEngine, MAX_CHANNELS } from './audio-engine.js';
import { peaksOf } from './wav.js';
import { DiskWriter, MemoryWriter, canUseFolders, pickFolder, downloadBlob, safeName } from './storage.js';
import { namesFromCsv, namesToCsv } from './csv.js';
import { LiveMixer, faderToDb, fmtDb, pluginName, defaults as stripDefaults } from './live.js';
import { loadWamModule, knownWams, rememberWam, serverWams, bundledWams } from './wam.js';
import { PLUGINS, PLUGIN_ORDER, formatParam } from './plugins.js';
import { TimecodeSource } from './timecode-source.js';
import { NetworkAudio } from './aes67.js';
import { bindNetworkPanel } from './aes67-panel.js';

const PEAK_STEP = 1024; // samples per waveform peak
const ROW_H = 64;
const RULER_H = 28;
const STORE_KEY = 'daw2110.tracks';

const $ = (id) => document.getElementById(id);
const engine = new AudioEngine();
const tc = new TimecodeSource(engine);
const net = new NetworkAudio(MAX_CHANNELS); // AES67 streams, received by the container
const AUTO_NAMES_KEY = 'daw2110.autonames';

const state = {
  open: false,
  recording: false,
  recFrames: 0,
  take: 0,
  folder: null,
  playhead: 0,
  pxPerSec: 40,
  writers: [], // [{ track, writer }] for the take being recorded
  selected: null, // index of the track whose clip is selected
  saving: false, // finishing files after a take
  dirty: true,
  mode: 'studio',
  editing: null, // { t, k }: plugin open in the editor
  editorGui: null, // { wam, gui }: a third-party plugin's own editor on screen
  serverWams: [], // plugins bundled with the server or in its plugins folder
  recBext: null, // Broadcast Wave stamp shared by the take's files, when timecode is locked
  recInfo: '', // take info text while recording
  recTc: '', // timecode of the take's first sample
};

const tracks = Array.from({ length: MAX_CHANNELS }, (_, i) => ({
  index: i,
  name: `Ch ${i + 1}`,
  armed: true,
  volume: 1,
  pan: 0,
  mute: false,
  solo: false,
  hasInput: true,
  buffer: null,
  peaks: [],
  peakRate: 48000 / PEAK_STEP,
  fileName: null,
  dir: null, // folder the clip's file was saved in, if any
  fileHandle: null,
  memWriter: null,
  meter: 0,
  hold: 0,
  holdUntil: 0,
  clip: false,
  el: null,
}));

let live; // LiveMixer, created in init()

// ---------- persistence ----------

function loadSaved() {
  try {
    const saved = JSON.parse(localStorage.getItem(STORE_KEY) || '[]');
    saved.forEach((s, i) => {
      if (!tracks[i] || !s) return;
      if (typeof s.name === 'string') tracks[i].name = s.name;
      if (typeof s.armed === 'boolean') tracks[i].armed = s.armed;
    });
  } catch (_) { /* storage unavailable */ }
}

function save() {
  try {
    localStorage.setItem(STORE_KEY, JSON.stringify(tracks.map((t) => ({ name: t.name, armed: t.armed }))));
  } catch (_) { /* storage unavailable */ }
  refreshStripNames();
}

// ---------- UI construction ----------

function buildTrackHeaders() {
  const tpl = $('track-template');
  const headers = $('headers');
  for (const t of tracks) {
    const el = tpl.content.firstElementChild.cloneNode(true);
    t.el = el;
    el.querySelector('.num').textContent = t.index + 1;
    const name = el.querySelector('.name');
    name.value = t.name;
    name.addEventListener('change', () => { t.name = name.value.trim() || `Ch ${t.index + 1}`; name.value = t.name; save(); });
    name.addEventListener('focus', () => name.select());
    name.addEventListener('keydown', (e) => {
      // Tab / Enter jump to the next channel name, Shift reverses.
      if (e.key !== 'Tab' && e.key !== 'Enter') return;
      e.preventDefault();
      const step = e.shiftKey ? -1 : 1;
      const next = tracks[t.index + step];
      if (next) next.el.querySelector('.name').focus();
      else name.blur();
    });

    el.querySelector('.arm').addEventListener('click', () => {
      if (state.recording) return;
      t.armed = !t.armed; save(); refreshTrack(t);
    });
    el.querySelector('.mute').addEventListener('click', () => { t.mute = !t.mute; refreshAll(); applyMix(); });
    el.querySelector('.solo').addEventListener('click', () => { t.solo = !t.solo; refreshAll(); applyMix(); });
    const vol = el.querySelector('.vol');
    const pan = el.querySelector('.pan');
    const label = () => {
      vol.title = `Playback volume ${gainText(t.volume)} (double-click for 0 dB)`;
      pan.title = `Pan ${panText(t.pan)} (double-click to center)`;
    };
    vol.addEventListener('input', () => { t.volume = +vol.value; applyMix(); label(); });
    vol.addEventListener('dblclick', () => { vol.value = 1; t.volume = 1; applyMix(); label(); });
    pan.addEventListener('input', () => { t.pan = +pan.value; applyMix(); label(); });
    pan.addEventListener('dblclick', () => { pan.value = 0; t.pan = 0; applyMix(); label(); });
    label();
    const resetPeak = () => { t.clip = false; t.hold = 0; };
    el.querySelector('.meter').addEventListener('click', resetPeak);
    el.querySelector('.db').addEventListener('click', resetPeak);
    headers.appendChild(el);
    refreshTrack(t);
  }
}

function refreshTrack(t) {
  t.el.querySelector('.arm').classList.toggle('on', t.armed);
  t.el.querySelector('.mute').classList.toggle('on', t.mute);
  t.el.querySelector('.solo').classList.toggle('on', t.solo);
  t.el.classList.toggle('no-input', !t.hasInput);
  const fed = net.trackName(t.index);
  t.el.title = !t.hasInput ? 'This input device has no channel for this track' : fed ? `AES67: ${fed}` : '';
  state.dirty = true;
}

function refreshAll() {
  tracks.forEach(refreshTrack);
}

function notice(msg, isError = false) {
  const n = $('notice');
  n.hidden = !msg;
  n.textContent = msg || '';
  n.classList.toggle('error', isError);
}

// ---------- controls ----------

function bindControls() {
  $('btn-enable').addEventListener('click', enableAudio);
  $('sel-input').addEventListener('change', openDevice);
  $('sel-rate').addEventListener('change', () => { if (state.open) openDevice(); });
  $('btn-folder').addEventListener('click', chooseFolder);
  $('btn-export').addEventListener('click', exportWavs);
  $('btn-rec').addEventListener('click', toggleRecord);
  $('btn-play').addEventListener('click', togglePlay);
  $('btn-stop').addEventListener('click', stopAll);
  $('btn-rtz').addEventListener('click', () => { setPlayhead(0); $('timeline').scrollLeft = 0; });
  $('btn-arm-all').addEventListener('click', () => { if (state.recording) return; tracks.forEach((t) => (t.armed = true)); save(); refreshAll(); });
  $('btn-arm-none').addEventListener('click', () => { if (state.recording) return; tracks.forEach((t) => (t.armed = false)); save(); refreshAll(); });
  $('btn-names-export').addEventListener('click', exportNames);
  $('btn-names-import').addEventListener('click', () => $('names-file').click());
  $('names-file').addEventListener('change', (e) => {
    const file = e.target.files[0];
    e.target.value = '';
    if (file) importNames(file);
  });
  $('btn-zoom-in').addEventListener('click', () => zoom(1.5));
  $('btn-zoom-out').addEventListener('click', () => zoom(1 / 1.5));
  const master = $('master-vol');
  const setMaster = () => {
    if (engine.master) engine.master.gain.value = +master.value;
    master.title = `Master playback volume ${gainText(+master.value)} (double-click for 0 dB)`;
  };
  master.addEventListener('input', setMaster);
  master.addEventListener('dblclick', () => { master.value = 1; setMaster(); });
  setMaster();
  $('timeline').addEventListener('scroll', () => (state.dirty = true));
  $('timeline-canvas').addEventListener('click', (e) => {
    if (state.recording) return;
    const t = ($('timeline').scrollLeft + e.offsetX) / state.pxPerSec;
    const row = Math.floor((e.offsetY - RULER_H) / ROW_H);
    const track = e.offsetY > RULER_H ? tracks[row] : null;
    select(track && track.buffer && t <= track.buffer.duration ? row : null);
    setPlayhead(Math.max(0, t));
  });
  $('btn-delete-clip').addEventListener('click', () => {
    if (state.selected !== null) deleteClips([tracks[state.selected]]);
  });
  window.addEventListener('resize', () => (state.dirty = true));

  window.addEventListener('keydown', (e) => {
    if (e.target.matches('input, select, textarea')) return;
    if (e.code === 'Space') { e.preventDefault(); state.recording ? stopAll() : togglePlay(); }
    else if (e.key === 'r' || e.key === 'R') { e.preventDefault(); toggleRecord(); }
    else if (e.key === 'Home' || e.key === 'Enter') { e.preventDefault(); $('btn-rtz').click(); }
    else if ((e.key === 'Delete' || e.key === 'Backspace') && state.selected !== null) { e.preventDefault(); $('btn-delete-clip').click(); }
    else if (e.key === 'Escape') select(null);
  });

  engine.addEventListener('meter', (e) => onMeter(e.detail.peaks));
  engine.addEventListener('data', (e) => onData(e.detail.data));
  engine.addEventListener('started', (e) => onRecordStarted(e.detail.frame));
  $('sel-tc').addEventListener('change', onTimecodeChoice);
  tc.addEventListener('devices', fillTimecodeMenu);
}

// ---------- timecode ----------

function fillTimecodeMenu() {
  const sel = $('sel-tc');
  const s = tc.setting;
  sel.textContent = '';
  const opt = (value, text, parent = sel) => {
    const o = document.createElement('option');
    o.value = value;
    o.textContent = text;
    parent.appendChild(o);
  };
  opt('off', 'Off');
  const ltc = document.createElement('optgroup');
  ltc.label = 'LTC (audio input)';
  for (let ch = 0; ch < MAX_CHANNELS; ch++) opt(`ltc:${ch}`, `LTC on input ${ch + 1}`, ltc);
  sel.appendChild(ltc);
  const mtc = document.createElement('optgroup');
  mtc.label = 'MTC (MIDI)';
  const inputs = tc.midiInputs();
  for (const i of inputs) opt(`mtc:${i.id}`, `MTC: ${i.name}`, mtc);
  if (s.type === 'mtc' && !inputs.some((i) => i.id === s.midi || i.name === s.midiName)) {
    opt(`mtc:${s.midi}`, `MTC: ${s.midiName || 'MIDI input'} (not connected)`, mtc);
  }
  if (!tc.midi) opt('mtc-ask', 'MTC from a MIDI input…', mtc);
  else if (!inputs.length) opt('mtc-ask', 'MTC: no MIDI inputs found', mtc);
  sel.appendChild(mtc);
  const cur = s.type === 'ltc' ? `ltc:${s.channel}` : s.type === 'mtc' ? `mtc:${s.midi}` : 'off';
  const match = [...sel.options].find((o) => o.value === cur)
    || (s.type === 'mtc' && [...sel.options].find((o) => o.textContent === `MTC: ${s.midiName}`));
  sel.value = match ? match.value : 'off';
}

async function onTimecodeChoice() {
  const v = $('sel-tc').value;
  if (v === 'off') await tc.set({ type: 'off' });
  else if (v.startsWith('ltc:')) await tc.set({ type: 'ltc', channel: +v.slice(4) });
  else if (v === 'mtc-ask') {
    try {
      await tc.requestMidi();
      const first = tc.midiInputs()[0];
      if (first) await tc.set({ type: 'mtc', midi: first.id, midiName: first.name });
      else notice('No MIDI inputs found. Connect your MIDI interface and choose Timecode again.');
    } catch (err) {
      notice(`Could not use MIDI: ${tc.midiError || err.message}`, true);
    }
  } else if (v.startsWith('mtc:')) {
    const id = v.slice(4);
    const input = tc.midiInputs().find((i) => i.id === id);
    await tc.set({ type: 'mtc', midi: id, midiName: input ? input.name : tc.setting.midiName });
  }
  fillTimecodeMenu();
  state.dirty = true;
}

// The first recorded sample's position is known: stamp the take's files with its timecode.
function onRecordStarted(frame) {
  if (!state.recBext) return;
  const st = tc.stampAt(frame);
  if (!st) return;
  Object.assign(state.recBext, {
    timeReference: st.timeReference,
    description: `sTC=${st.label} fps=${st.rate.id}`,
  });
  state.recTc = st.label;
}

function drawTimecode() {
  const clock = $('clock');
  const status = $('tc-status');
  const on = tc.active;
  clock.classList.toggle('tc', on);
  status.hidden = !on;
  if (!on) { clock.classList.remove('tc-lost'); return false; }
  clock.textContent = tc.text();
  clock.classList.toggle('tc-lost', !tc.receiving);
  const st = tc.status();
  if (status.textContent !== st) status.textContent = st;
  status.classList.toggle('ok', tc.locked);
  return true;
}

function bextStamp() {
  const now = new Date();
  const p = (n) => String(n).padStart(2, '0');
  return {
    description: '',
    originator: 'Soundbending Live',
    date: `${now.getFullYear()}-${p(now.getMonth() + 1)}-${p(now.getDate())}`,
    time: `${p(now.getHours())}:${p(now.getMinutes())}:${p(now.getSeconds())}`,
    timeReference: 0, // filled in when the first sample's position is known
  };
}

async function enableAudio() {
  try {
    notice('');
    await engine.unlockDeviceLabels();
    const inputs = await engine.listInputs();
    const sel = $('sel-input');
    sel.innerHTML = '';
    for (const d of inputs) {
      const o = document.createElement('option');
      o.value = d.deviceId;
      o.textContent = d.label || `Input ${sel.length + 1}`;
      sel.appendChild(o);
    }
    sel.disabled = false;
    await listOutputs();
    $('btn-enable').textContent = 'Refresh devices';
    $('btn-enable').classList.remove('primary');
    await openDevice();
  } catch (err) {
    notice(`Could not access audio input: ${err.message}`, true);
  }
}

async function openDevice() {
  if (state.recording) return;
  stopPlayback();
  try {
    const rate = +$('sel-rate').value || undefined;
    const info = await engine.open({ deviceId: $('sel-input').value, sampleRate: rate });
    state.open = true;
    engine.master.gain.value = +$('master-vol').value; // keep master level across device changes
    updateInputs();
    $('live-latency').textContent = engine.latency ? `Latency ≈ ${Math.round(engine.latency * 1000)} ms` : '';
    if (info.channels < MAX_CHANNELS && !net.subs.length) {
      notice(`This input gives the browser ${info.channels} channel${info.channels === 1 ? '' : 's'}, so tracks ${info.channels + 1}–${MAX_CHANNELS} will record silence. ` +
        `For all ${MAX_CHANNELS} channels, use Chrome with an interface that appears as one multichannel device (see README).`);
    } else {
      notice('');
    }
    // Clips recorded at another sample rate are dropped; they would play at the wrong speed.
    for (const t of tracks) if (t.buffer && t.buffer.sampleRate !== info.sampleRate) { t.buffer = null; t.peaks = []; }
    state.dirty = true;
  } catch (err) {
    state.open = false;
    notice(`Could not open that input: ${err.message}`, true);
  }
}

// Which tracks have something feeding them: the interface's channels, or a network stream.
function updateInputs() {
  let changed = false;
  for (const t of tracks) {
    const has = !state.open || t.index < engine.deviceChannels || !!net.sourceOf(t.index);
    const title = net.trackName(t.index);
    if (t.hasInput !== has || t.fedBy !== title) { t.hasInput = has; t.fedBy = title; changed = true; }
  }
  if (state.open) {
    const netCh = tracks.filter((t) => net.sourceOf(t.index)).length;
    const missing = tracks.filter((t) => !t.hasInput).length;
    $('device-info').textContent = `${engine.deviceChannels} ch · ${(engine.sampleRate / 1000).toFixed(1)} kHz` +
      (netCh ? ` · ${netCh} AES67` : '') + (missing ? ` · ${missing} track${missing === 1 ? ' has' : 's have'} no input` : '');
    if (netCh && $('notice').textContent.startsWith('This input gives the browser')) notice('');
  }
  if (!changed) return;
  refreshAll();
  tracks.forEach((t) => t.strip.classList.toggle('no-input', !t.hasInput));
}

// Tracks fed by a network stream take its channel names ("Stage Box A 3"),
// unless you've typed a name of your own.
function applyAutoNames() {
  let auto = [];
  try { auto = JSON.parse(localStorage.getItem(AUTO_NAMES_KEY) || '[]'); } catch (_) { /* storage unavailable */ }
  let changed = false;
  for (const t of tracks) {
    const want = net.trackName(t.index).slice(0, NAME_MAX);
    const was = auto[t.index] || '';
    if (want === was) continue;
    const automatic = t.name === `Ch ${t.index + 1}` || (was && t.name === was);
    if (automatic) {
      t.name = want || `Ch ${t.index + 1}`;
      t.el.querySelector('.name').value = t.name;
    }
    auto[t.index] = want;
    changed = true;
  }
  if (!changed) return;
  try { localStorage.setItem(AUTO_NAMES_KEY, JSON.stringify(auto)); } catch (_) { /* storage unavailable */ }
  save();
}

function onNetworkChange() {
  updateInputs();
  applyAutoNames();
  const n = net.subs.length;
  const btn = $('btn-aes67');
  const txt = n ? `AES67 · ${n} stream${n === 1 ? '' : 's'}` : 'AES67 network';
  if (btn.textContent !== txt) btn.textContent = txt;
}

async function chooseFolder() {
  try {
    state.folder = await pickFolder();
    $('folder-info').textContent = `Saving takes to “${state.folder.name}”`;
  } catch (err) {
    if (err.name !== 'AbortError') notice(`Could not use that folder: ${err.message}`, true);
  }
}

function zoom(f) {
  const tl = $('timeline');
  const center = (tl.scrollLeft + tl.clientWidth / 2) / state.pxPerSec;
  state.pxPerSec = Math.min(400, Math.max(2, state.pxPerSec * f));
  layoutTimeline();
  tl.scrollLeft = center * state.pxPerSec - tl.clientWidth / 2;
  state.dirty = true;
}

// ---------- mixing & playback ----------

function trackGain(t) {
  const anySolo = tracks.some((x) => x.solo);
  if (t.mute || (anySolo && !t.solo)) return 0;
  return t.volume;
}

function applyMix() {
  if (!engine.playing) return;
  tracks.forEach((t, i) => engine.setVoice(i, { gain: trackGain(t), pan: t.pan }));
}

function hasClips() {
  return tracks.some((t) => t.buffer);
}

function togglePlay() {
  if (state.recording || !state.open) return;
  if (engine.playing) { stopPlayback(); return; }
  if (!hasClips()) { notice('Nothing to play yet. Arm some tracks and press record.'); return; }
  engine.play(tracks.map((t) => ({ buffer: t.buffer, gain: trackGain(t), pan: t.pan })), state.playhead);
  $('btn-play').classList.add('on');
}

function stopPlayback() {
  if (engine.playing) state.playhead = engine.playPosition;
  engine.stopPlayback();
  $('btn-play').classList.remove('on');
  state.dirty = true;
}

function setPlayhead(t) {
  const wasPlaying = engine.playing;
  stopPlayback();
  state.playhead = t;
  if (wasPlaying) togglePlay();
  state.dirty = true;
}

function stopAll() {
  if (state.recording) { stopRecording(); return; }
  stopPlayback();
}

// ---------- recording ----------

async function toggleRecord() {
  if (state.recording) return stopRecording();
  if (state.saving) return;
  if (!state.open) { notice('Click “Enable audio” and pick your interface first.'); return; }
  const armed = tracks.filter((t) => t.armed);
  if (!armed.length) { notice('No tracks are armed. Click ● on the tracks you want to record.'); return; }

  stopPlayback();
  notice(state.folder ? '' : (canUseFolders
    ? 'Recording to memory. For long takes, choose a save folder first so audio streams to disk.'
    : ''));

  const take = state.take + 1;
  const sr = engine.sampleRate;
  const tag = `T${String(take).padStart(2, '0')}`;
  // With timecode locked, every file of the take gets a Broadcast Wave stamp.
  const bext = tc.locked ? bextStamp() : null;
  try {
    state.writers = await Promise.all(armed.map(async (t) => {
      const fileName = `${tag}_${String(t.index + 1).padStart(2, '0')}_${safeName(t.name)}.wav`;
      const writer = state.folder
        ? await DiskWriter.create(state.folder, fileName, sr, bext)
        : new MemoryWriter(fileName, sr, bext);
      return { track: t, writer, fileName };
    }));
  } catch (err) {
    notice(`Could not create files in the save folder: ${err.message}`, true);
    return;
  }

  state.take = take;
  state.recBext = bext;
  state.recTc = '';
  for (const { track, fileName } of state.writers) {
    track.buffer = null;
    track.peaks = [];
    track.peakRate = sr / PEAK_STEP;
    track.fileName = fileName;
    track.memWriter = null;
    track.dir = state.folder;
    track.fileHandle = null;
  }
  select(null);
  state.recording = true;
  state.recFrames = 0;
  state.playhead = 0;
  $('timeline').scrollLeft = 0;
  $('btn-rec').classList.add('on');
  state.recInfo = `Recording take ${take} · ${armed.length} track${armed.length === 1 ? '' : 's'}`;
  $('take-info').textContent = state.recInfo;
  engine.startRecording(armed.map((t) => t.index));
  state.dirty = true;
}

function onData(data) {
  if (!state.recording) return;
  data.forEach((chunk, k) => {
    const w = state.writers[k];
    if (!w) return;
    w.writer.write(chunk);
    const p = peaksOf(chunk, PEAK_STEP);
    for (let i = 0; i < p.length; i++) w.track.peaks.push(p[i]);
  });
  state.recFrames += data[0] ? data[0].length : 0;
  state.dirty = true;
}

async function stopRecording() {
  if (!state.recording) return;
  await engine.stopRecording();
  state.recording = false;
  state.saving = true;
  $('btn-rec').classList.remove('on');
  const seconds = state.recFrames / engine.sampleRate;
  $('take-info').textContent = `Saving take ${state.take}…`;

  const problems = [];
  let memory = false;
  for (const { track, writer } of state.writers) {
    try {
      await writer.close();
      if (writer instanceof MemoryWriter) {
        track.buffer = writer.toAudioBuffer(engine.ctx);
        track.memWriter = writer;
        memory = true;
      } else {
        track.fileHandle = writer.handle;
        track.buffer = await engine.decode(await writer.readArrayBuffer());
      }
    } catch (err) {
      problems.push(`${track.name}: ${err.message}`);
    }
  }
  state.writers = [];
  state.saving = false;
  refreshClipButtons();
  $('take-info').textContent = `Take ${state.take} · ${formatTime(seconds)}` +
    (state.recTc ? ` · TC ${state.recTc}` : '') +
    (state.folder && !memory ? ` · saved to “${state.folder.name}”` : '');
  state.recBext = null;
  if (problems.length) notice(`Some tracks failed to save: ${problems.join('; ')}`, true);
  state.dirty = true;
}

// ---------- modes ----------

function loadMode() {
  try { return localStorage.getItem('daw2110.mode') === 'live' ? 'live' : 'studio'; } catch (_) { return 'studio'; }
}

function setMode(mode) {
  state.mode = mode;
  document.querySelectorAll('.mode').forEach((b) => b.classList.toggle('active', b.dataset.mode === mode));
  document.querySelectorAll('.studio-only').forEach((el) => (el.hidden = mode !== 'studio'));
  document.querySelectorAll('.live-only').forEach((el) => (el.hidden = mode !== 'live'));
  try { localStorage.setItem('daw2110.mode', mode); } catch (_) { /* storage unavailable */ }
  state.dirty = true;
}

// ---------- live console ----------

const KNOB_TEXT = {
  trim: (v) => `${v > 0 ? '+' : ''}${v.toFixed(1)}`,
  pan: (v) => panText(v).replace('center', 'C'),
};
const KNOB_TITLE = { trim: 'Input gain (dB)', pan: 'Pan' };

function buildStrips() {
  const tpl = $('strip-template');
  const consoleEl = $('console');
  for (const t of tracks) {
    const el = tpl.content.firstElementChild.cloneNode(true);
    t.strip = el;
    t.live = { meter: 0, hold: 0, holdUntil: 0, clip: false };
    el.querySelector('.strip-num').textContent = t.index + 1;
    const p = () => live.params[t.index];
    const sync = () => {
      el.querySelectorAll('input[data-key]').forEach((inp) => {
        const key = inp.dataset.key;
        inp.value = p()[key];
        const out = inp.closest('.knob')?.querySelector('output');
        if (out) out.textContent = KNOB_TEXT[key](p()[key]);
        if (KNOB_TITLE[key]) inp.title = `${KNOB_TITLE[key]}: ${KNOB_TEXT[key](p()[key])}`;
      });
      el.querySelector('.fader-db').textContent = fmtDb(faderToDb(p().fader));
      el.querySelector('.fader').title = `Fader ${fmtDb(faderToDb(p().fader))} dB (double-click for 0 dB)`;
      ['mute', 'solo'].forEach((k) => el.querySelector(`.${k}`).classList.toggle('on', p()[k]));
      renderInserts(t);
    };
    const add = el.querySelector('.add-plugin');
    fillAddMenu(add);
    add.addEventListener('change', () => {
      const v = add.value;
      add.value = '';
      if (!v) return;
      if (v === 'wam-url') { addWamFromPrompt(t); return; }
      if (v.startsWith('wam:')) { addWam(t, v.slice(4)); return; }
      const k = live.addPlugin(t.index, v);
      renderInserts(t);
      openPlugin(t, k);
    });
    el.querySelectorAll('input[data-key]').forEach((inp) => {
      inp.addEventListener('input', () => { live.set(t.index, inp.dataset.key, +inp.value); sync(); });
      inp.addEventListener('dblclick', () => { live.set(t.index, inp.dataset.key, stripDefaults()[inp.dataset.key]); sync(); });
    });
    el.querySelectorAll('button[data-key]').forEach((b) => {
      b.addEventListener('click', () => {
        live.set(t.index, b.dataset.key, !p()[b.dataset.key]);
        if (b.dataset.key === 'solo') tracks.forEach((x) => x.syncStrip()); else sync();
      });
    });
    el.querySelector('.vmeter').addEventListener('click', () => { t.live.clip = false; t.live.hold = 0; });
    t.syncStrip = sync;
    sync();
    consoleEl.appendChild(el);
  }
  refreshStripNames();
}

// The + Plugin menu: built-in plugins, then third-party Web Audio Modules.
function fillAddMenu(sel) {
  sel.textContent = '';
  const opt = (value, text, parent = sel) => {
    const o = document.createElement('option');
    o.value = value;
    o.textContent = text;
    parent.appendChild(o);
  };
  opt('', '+ Plugin');
  const builtIn = document.createElement('optgroup');
  builtIn.label = 'Built-in';
  for (const type of PLUGIN_ORDER) opt(type, PLUGINS[type].name, builtIn);
  sel.appendChild(builtIn);
  const third = document.createElement('optgroup');
  third.label = 'Web Audio Modules';
  const seen = new Set();
  for (const w of [...state.serverWams, ...knownWams()]) {
    if (seen.has(w.url)) continue;
    seen.add(w.url);
    opt(`wam:${w.url}`, w.name, third);
  }
  opt('wam-url', 'Load plugin from URL…', third);
  sel.appendChild(third);
}

function refreshAddMenus() {
  for (const t of tracks) if (t.strip) fillAddMenu(t.strip.querySelector('.add-plugin'));
}

function addWamFromPrompt(t) {
  const url = (window.prompt('Address of a Web Audio Module plugin (its index.js):', 'https://') || '').trim();
  if (url && url !== 'https://') addWam(t, url);
}

async function addWam(t, url) {
  try {
    await loadWamModule(url);
  } catch (e) {
    notice(`Couldn't load the plugin at ${url} (${e.message}). Check the address. Plugins on other websites only load if that site allows it (CORS); if not, copy the plugin into the plugins folder.`, true);
    return;
  }
  notice('');
  const k = live.addPlugin(t.index, 'wam', url);
  rememberWam(url, live.params[t.index].plugins[k].name);
  refreshAddMenus();
  renderInserts(t);
  openPlugin(t, k);
}

// A third-party plugin finished loading (or failed): show its name and editor.
function onWamReady(i, pl) {
  const t = tracks[i];
  if (!t || !t.strip) return;
  const known = knownWams().find((w) => w.url === pl.url);
  if (pl.name && known && known.name !== pl.name) { rememberWam(pl.url, pl.name); refreshAddMenus(); }
  renderInserts(t);
  if (state.editing && state.editing.t === t && live.params[i].plugins[state.editing.k] === pl) openPlugin(t, state.editing.k);
}

function renderInserts(t) {
  const list = t.strip.querySelector('.insert-list');
  list.textContent = '';
  live.params[t.index].plugins.forEach((pl, k) => {
    const b = document.createElement('button');
    b.className = 'insert';
    b.textContent = pluginName(pl);
    b.title = `${pluginName(pl)}${pl.bypass ? ' (bypassed)' : ''}: click to edit`;
    b.classList.toggle('bypassed', pl.bypass);
    b.classList.toggle('editing', state.editing && state.editing.t === t && state.editing.k === k);
    b.addEventListener('click', () => openPlugin(t, k));
    list.appendChild(b);
  });
}

// ---------- plugin editor ----------

function openPlugin(t, k) {
  const prev = state.editing && state.editing.t;
  dropWamGui();
  state.editing = { t, k };
  if (prev && prev !== t) renderInserts(prev);
  renderInserts(t);
  const pl = live.params[t.index].plugins[k];
  $('pp-title').textContent = `${pluginName(pl)} · ${t.index + 1} ${t.name}`;
  const box = $('pp-params');
  box.textContent = '';
  $('pp-bypass').classList.toggle('on', pl.bypass);
  $('pp-bypass').textContent = pl.bypass ? 'Bypassed' : 'Bypass';
  $('pp-up').disabled = k === 0;
  $('pp-down').disabled = k === live.params[t.index].plugins.length - 1;
  $('plugin-panel').hidden = false;
  $('plugin-panel').classList.toggle('wide', pl.type === 'wam');
  if (pl.type === 'wam') { showWam(t, k, pl, box); return; }
  const def = PLUGINS[pl.type];
  for (const p of def.params) {
    const row = document.createElement('label');
    row.className = 'pp-param';
    const name = document.createElement('span');
    name.textContent = p.label;
    const inp = document.createElement('input');
    Object.assign(inp, { type: 'range', min: p.min, max: p.max, step: p.step, value: pl.values[p.key] });
    const out = document.createElement('output');
    out.textContent = formatParam(p, pl.values[p.key]);
    inp.addEventListener('input', () => {
      live.setPluginParam(t.index, state.editing.k, p.key, +inp.value);
      out.textContent = formatParam(p, +inp.value);
    });
    inp.addEventListener('dblclick', () => {
      inp.value = p.def;
      live.setPluginParam(t.index, state.editing.k, p.key, p.def);
      out.textContent = formatParam(p, p.def);
    });
    row.append(name, inp, out);
    box.appendChild(row);
  }
}

function paramRow(label, min, max, step, value, fmt, onInput) {
  const row = document.createElement('label');
  row.className = 'pp-param';
  const name = document.createElement('span');
  name.textContent = label;
  const inp = document.createElement('input');
  Object.assign(inp, { type: 'range', min, max, step, value });
  const out = document.createElement('output');
  out.textContent = fmt(value);
  inp.addEventListener('input', () => { onInput(+inp.value); out.textContent = fmt(+inp.value); });
  row.append(name, inp, out);
  return row;
}

// Editor for a third-party plugin: its own interface if it has one,
// otherwise a slider per parameter.
async function showWam(t, k, pl, box) {
  const msg = (text) => { const p = document.createElement('p'); p.className = 'muted pp-msg'; p.textContent = text; box.appendChild(p); };
  const inst = live.insert(t.index, k);
  if (!inst) { msg('Click “Enable audio” to load this plugin.'); return; }
  if (inst.error) { msg(`This plugin didn't load: ${inst.error}`); return; }
  if (!inst.wam) { msg('Loading plugin…'); return; } // reopened when it's ready
  const still = () => state.editing && state.editing.t === t && state.editing.k === k;
  const { wam } = inst;
  let gui = null;
  try { gui = await wam.createGui(); } catch (_) { gui = null; }
  if (!still()) { if (gui) wam.destroyGui?.(gui); return; }
  box.textContent = '';
  if (gui) {
    const wrap = document.createElement('div');
    wrap.className = 'pp-gui';
    wrap.appendChild(gui);
    box.appendChild(wrap);
    state.editorGui = { wam, gui };
    return;
  }
  let info = {};
  let values = {};
  try {
    info = await wam.audioNode.getParameterInfo();
    values = await wam.audioNode.getParameterValues(false);
  } catch (_) { /* no parameters */ }
  if (!still()) return;
  const params = Object.values(info);
  if (!params.length) { msg('This plugin has no settings to show.'); return; }
  for (const p of params) {
    const step = p.discreteStep || (p.maxValue - p.minValue) / 1000;
    const v = values[p.id] ? values[p.id].value : p.defaultValue;
    const fmt = (x) => `${Number.isInteger(step) ? Math.round(x) : x.toFixed(2)}${p.units ? ` ${p.units}` : ''}`;
    box.appendChild(paramRow(p.label || p.id, p.minValue, p.maxValue, step, v, fmt, (x) => {
      wam.audioNode.setParameterValues({ [p.id]: { id: p.id, value: x, normalized: false } });
    }));
  }
}

function dropWamGui() {
  const g = state.editorGui;
  state.editorGui = null;
  if (!g) return;
  g.gui.remove();
  try { g.wam.destroyGui?.(g.gui); } catch (_) { /* plugin cleanup failed */ }
}

function closePlugin() {
  const t = state.editing && state.editing.t;
  dropWamGui();
  live.saveWamStates();
  state.editing = null;
  $('plugin-panel').hidden = true;
  if (t) renderInserts(t);
}

function bindPluginPanel() {
  $('pp-close').addEventListener('click', closePlugin);
  $('pp-bypass').addEventListener('click', () => {
    const { t, k } = state.editing;
    live.setBypass(t.index, k, !live.params[t.index].plugins[k].bypass);
    openPlugin(t, k);
  });
  $('pp-up').addEventListener('click', () => { const { t, k } = state.editing; openPlugin(t, live.movePlugin(t.index, k, -1)); });
  $('pp-down').addEventListener('click', () => { const { t, k } = state.editing; openPlugin(t, live.movePlugin(t.index, k, 1)); });
  $('pp-remove').addEventListener('click', () => {
    const { t, k } = state.editing;
    live.removePlugin(t.index, k);
    closePlugin();
  });
  window.addEventListener('keydown', (e) => { if (e.key === 'Escape' && state.editing) closePlugin(); });
}

function drawPluginMeter() {
  if (!state.editing) return;
  const gr = live.pluginMeter(state.editing.t.index, state.editing.k);
  const text = gr === null ? '' : `Gain reduction ${gr.toFixed(1)} dB`;
  if ($('pp-gr').textContent !== text) $('pp-gr').textContent = text;
}

function refreshStripNames() {
  for (const t of tracks) {
    if (!t.strip) continue;
    const n = t.strip.querySelector('.strip-name');
    if (n.textContent !== t.name) { n.textContent = t.name; n.title = t.name; }
  }
}

function bindLive() {
  document.querySelectorAll('.mode').forEach((b) => b.addEventListener('click', () => {
    if (!b.disabled) setMode(b.dataset.mode);
  }));
  $('btn-live').addEventListener('click', () => {
    if (!state.open) { notice('Click “Enable audio” and pick your interface first.'); return; }
    live.setOn(!live.on);
    refreshLiveStatus();
  });
  const lim = $('chk-limiter');
  lim.checked = engine.limiterOn;
  lim.addEventListener('change', () => { engine.setLimiter(lim.checked); live.save(); });
  $('sel-output').addEventListener('change', async (e) => {
    try {
      await engine.setOutput(e.target.value);
      live.save();
      refreshLiveStatus();
    } catch (err) {
      notice(`Could not switch output: ${err.message}`, true);
    }
  });
  if (!engine.canChooseOutput) $('sel-output').title = 'Choosing an output needs Chrome 110 or newer';
}

async function listOutputs() {
  const sel = $('sel-output');
  if (!engine.canChooseOutput) return;
  const outs = await engine.listOutputs();
  sel.innerHTML = '';
  for (const d of outs) {
    if (d.deviceId === 'default' || d.deviceId === 'communications') continue;
    const o = document.createElement('option');
    o.value = d.deviceId;
    o.textContent = d.label || 'Output';
    sel.appendChild(o);
  }
  const def = document.createElement('option');
  def.value = '';
  def.textContent = 'Default output';
  sel.prepend(def);
  sel.value = [...sel.options].some((o) => o.value === engine.sinkId) ? engine.sinkId : '';
  sel.disabled = false;
}

function refreshLiveStatus() {
  const btn = $('btn-live');
  btn.classList.toggle('on', live.on);
  btn.textContent = live.on ? 'LIVE' : 'Go live';
  const sel = $('sel-output');
  const out = sel.options[sel.selectedIndex]?.textContent || 'the default output';
  $('live-status').textContent = live.on ? `Live mix is going to ${out}` : 'Live mix is off';
}

const FADER_MARKS = [10, 5, 0, -5, -10, -20, -30, -40, -60];
const FADER_THUMB = 46; // px, keep in sync with .fader thumb height in CSS

// dB scale printed beside each live fader; drawn once per size change.
function drawFaderScale(c) {
  if (c.clientHeight === c._drawnH) return;
  c._drawnH = c.clientHeight;
  const { g, w, h } = fitCanvas(c);
  g.clearRect(0, 0, w, h);
  g.font = '9px ui-monospace, Menlo, Consolas, monospace';
  g.textAlign = 'right';
  g.textBaseline = 'middle';
  const top = FADER_THUMB / 2, travel = h - FADER_THUMB;
  for (const db of FADER_MARKS) {
    const y = Math.round(top + (1 - dbToFaderPos(db)) * travel) + 0.5;
    g.fillStyle = db === 0 ? '#e6e8ee' : '#8b92a3';
    g.fillText(db > 0 ? `+${db}` : String(db), w - 6, y);
    g.fillRect(w - 4, y, 4, 1);
  }
}

function dbToFaderPos(db) {
  return (db + 60) / 70;
}

function drawLiveMeters() {
  drawPluginMeter();
  for (const t of tracks) drawFaderScale(t.strip.querySelector('.fader-scale'));
  const now = performance.now();
  for (const t of tracks) {
    feedMeter(t.live, live.peak(t.index), now);
    const { g, w, h } = fitCanvas(t.strip.querySelector('.vmeter'));
    // vertical bar: bottom = -60 dB, top = 0 dB
    g.fillStyle = '#0c0d10';
    g.fillRect(0, 0, w, h);
    const lvl = dbPos(t.live.meter) * h;
    const grad = g.createLinearGradient(0, h, 0, 0);
    grad.addColorStop(0, '#2ed573');
    grad.addColorStop(dbPos(10 ** (-12 / 20)), '#2ed573');
    grad.addColorStop(dbPos(10 ** (-6 / 20)), '#f5c542');
    grad.addColorStop(1, '#ff4757');
    g.fillStyle = grad;
    g.fillRect(0, h - lvl, w, lvl);
    for (const db of METER_TICKS) {
      g.fillStyle = 'rgba(12,13,16,0.85)';
      g.fillRect(0, h - tickX(db, h), w, 1);
    }
    const hy = dbPos(t.live.hold) * h;
    if (hy > 1) { g.fillStyle = '#e6e8ee'; g.fillRect(0, h - hy, w, 2); }
    if (t.live.clip) { g.fillStyle = '#ff4757'; g.fillRect(0, 0, w, 5); }
    t.live.meter *= 0.88;
  }
}

// ---------- channel names CSV ----------

const NAME_MAX = 24;

function exportNames() {
  const blob = new Blob([namesToCsv(tracks.map((t) => t.name))], { type: 'text/csv' });
  downloadBlob(blob, 'channel-names.csv');
}

async function importNames(file) {
  try {
    const entries = namesFromCsv(await file.text());
    let count = 0;
    for (const { channel, name } of entries) {
      const t = tracks[channel - 1];
      if (!t) continue;
      t.name = name.slice(0, NAME_MAX);
      t.el.querySelector('.name').value = t.name;
      count++;
    }
    save();
    state.dirty = true;
    notice(count
      ? `Named ${count} channel${count === 1 ? '' : 's'} from ${file.name}.`
      : `No channel names found in ${file.name}. Use two columns: Channel, Name.`, !count);
  } catch (err) {
    notice(`Could not read ${file.name}: ${err.message}`, true);
  }
}

// ---------- deleting ----------

function select(index) {
  state.selected = index;
  refreshClipButtons();
  state.dirty = true;
}

function refreshClipButtons() {
  const busy = state.recording || state.saving;
  $('btn-delete-clip').disabled = busy || state.selected === null;
  $('btn-export').disabled = busy || !tracks.some((t) => t.memWriter);
}

// Removes clips from the timeline right away. Their WAV files are moved into a
// "Deleted" subfolder of the save folder rather than erased, so a slip of the
// Delete key never loses a recording for good.
async function deleteClips(list) {
  if (state.recording || state.saving || !list.length) return;
  stopPlayback();
  const kept = [];
  for (const t of list) {
    if (t.dir && t.fileHandle) {
      try {
        if (typeof t.fileHandle.move !== 'function') throw new Error('not supported');
        const trash = await t.dir.getDirectoryHandle('Deleted', { create: true });
        await t.fileHandle.move(trash);
      } catch (_) {
        kept.push(t.fileName);
      }
    }
    Object.assign(t, { buffer: null, peaks: [], fileName: null, memWriter: null, dir: null, fileHandle: null });
  }
  select(null);
  if (!hasClips()) { state.playhead = 0; $('take-info').textContent = 'No takes yet'; }
  notice(kept.length ? `Removed from the timeline. This browser couldn't move ${kept.join(', ')} into the Deleted folder, so the file is still in your save folder.` : '');
  state.dirty = true;
}

function exportWavs() {
  const list = tracks.filter((t) => t.memWriter);
  list.forEach((t, i) => setTimeout(() => downloadBlob(t.memWriter.toBlob(), t.fileName), i * 300));
}

function gainText(g) {
  if (g <= 0) return '-∞ dB';
  const db = 20 * Math.log10(g);
  return `${db > 0.05 ? '+' : ''}${Math.abs(db) < 0.05 ? '0.0' : db.toFixed(1)} dB`;
}

function panText(p) {
  const n = Math.round(Math.abs(p) * 100);
  return n === 0 ? 'center' : `${p < 0 ? 'L' : 'R'}${n}`;
}

// ---------- meters ----------

function onMeter(peaks) {
  const now = performance.now();
  tracks.forEach((t, i) => feedMeter(t, peaks[i] || 0, now));
}

const METER_FLOOR = -60; // dB at the left edge of a meter
const METER_TICKS = [-48, -36, -24, -18, -12, -6, -3, 0];

function toDb(v) {
  return v > 0 ? 20 * Math.log10(v) : -Infinity;
}

function dbPos(v) {
  const db = toDb(v);
  return Math.min(1, Math.max(0, (db - METER_FLOOR) / -METER_FLOOR));
}

function dbText(v) {
  const db = toDb(v);
  if (db < METER_FLOOR) return '-∞';
  return db >= -0.05 ? '0.0' : db.toFixed(1);
}

function fitCanvas(c) {
  const dpr = window.devicePixelRatio || 1;
  const cw = c.clientWidth, ch = c.clientHeight;
  if (c.width !== Math.round(cw * dpr) || c.height !== Math.round(ch * dpr)) {
    c.width = Math.round(cw * dpr);
    c.height = Math.round(ch * dpr);
  }
  const g = c.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { g, w: cw, h: ch };
}

const tickX = (db, w) => Math.round(((db - METER_FLOOR) / -METER_FLOOR) * w);

// One horizontal meter bar: level with fall-off, peak-hold line, clip light.
function drawBar(g, x0, y0, w, h, m) {
  g.fillStyle = '#16181d';
  g.fillRect(x0, y0, w, h);
  const grad = g.createLinearGradient(x0, 0, x0 + w, 0);
  grad.addColorStop(0, '#2ed573');
  grad.addColorStop(dbPos(10 ** (-12 / 20)), '#2ed573');
  grad.addColorStop(dbPos(10 ** (-6 / 20)), '#f5c542');
  grad.addColorStop(1, '#ff4757');
  g.fillStyle = grad;
  g.fillRect(x0, y0, dbPos(m.meter) * w, h);
  for (const db of METER_TICKS) {
    g.fillStyle = 'rgba(12,13,16,0.85)';
    g.fillRect(x0 + tickX(db, w) - 1, y0, 1, h);
  }
  const hx = dbPos(m.hold) * w;
  if (hx > 1) { g.fillStyle = '#e6e8ee'; g.fillRect(x0 + hx - 1, y0, 2, h); }
  if (m.clip) { g.fillStyle = '#ff4757'; g.fillRect(x0 + w - 5, y0, 5, h); }
}

function setReadout(el, m) {
  const text = dbText(m.hold);
  if (el.textContent !== text) el.textContent = text;
  el.classList.toggle('clip', m.clip);
}

// Feed a new peak into a meter's state ({ meter, hold, holdUntil, clip }).
function feedMeter(m, p, now) {
  if (p > m.meter) m.meter = p;
  if (p >= m.hold || now > m.holdUntil) { m.hold = p; m.holdUntil = now + 1500; }
  if (p >= 0.999) m.clip = true;
}

const masterMeters = [0, 1].map(() => ({ meter: 0, hold: 0, holdUntil: 0, clip: false }));
document.getElementById('master-meter').addEventListener('click', () => {
  masterMeters.forEach((m) => { m.clip = false; m.hold = 0; });
});

function drawMasterMeter() {
  const now = performance.now();
  engine.masterPeaks().forEach((p, i) => feedMeter(masterMeters[i], p, now));
  const { g, w, h } = fitCanvas($('master-meter'));
  g.clearRect(0, 0, w, h);
  const barH = 10;
  drawBar(g, 0, 0, w, barH, masterMeters[0]);
  drawBar(g, 0, h - barH, w, barH, masterMeters[1]);
  // scale between the bars
  g.font = '9px ui-monospace, Menlo, Consolas, monospace';
  g.textBaseline = 'middle';
  g.fillStyle = '#8b92a3';
  let lastRight = -Infinity;
  for (const db of METER_TICKS) {
    const label = String(db);
    const tw = g.measureText(label).width;
    const lx = Math.min(w - tw, Math.max(0, tickX(db, w) - tw / 2));
    if (lx < lastRight + 4 && db !== 0) continue; // skip labels that would collide
    g.fillText(label, lx, h / 2);
    lastRight = lx + tw;
  }
  setReadout($('master-db-l'), masterMeters[0]);
  setReadout($('master-db-r'), masterMeters[1]);
  masterMeters.forEach((m) => (m.meter *= 0.88));
}

function drawMeters() {
  if (state.mode === 'live') drawLiveMeters();
  for (const t of state.mode === 'studio' ? tracks : []) {
    const { g, w, h } = fitCanvas(t.el.querySelector('.meter'));
    drawBar(g, 0, 0, w, h, t);
    for (const db of METER_TICKS) {
      g.fillStyle = 'rgba(139,146,163,0.55)';
      g.fillRect(tickX(db, w) - 1, h - 3, 1, 3);
    }
    setReadout(t.el.querySelector('.db'), t);
    t.meter *= 0.88; // fall-off
  }
  drawMasterMeter();
}

// ---------- timeline ----------

function totalSeconds() {
  let s = state.recording ? state.recFrames / engine.sampleRate : 0;
  for (const t of tracks) if (t.buffer) s = Math.max(s, t.buffer.duration);
  return s;
}

function layoutTimeline() {
  const tl = $('timeline');
  const width = Math.max(tl.clientWidth, (totalSeconds() + 30) * state.pxPerSec);
  const content = $('timeline-content');
  content.style.width = `${width}px`;
  content.style.height = `${RULER_H + ROW_H * tracks.length}px`;
}

function niceInterval(pps) {
  for (const s of [0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600]) if (s * pps >= 70) return s;
  return 1200;
}

function drawTimeline(pos) {
  layoutTimeline();
  const tl = $('timeline');
  const canvas = $('timeline-canvas');
  const dpr = window.devicePixelRatio || 1;
  const w = tl.clientWidth;
  const h = RULER_H + ROW_H * tracks.length;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
    canvas.style.width = `${w}px`;
    canvas.style.height = `${h}px`;
  }
  const g = canvas.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  const pps = state.pxPerSec;
  const t0 = tl.scrollLeft / pps;

  g.fillStyle = '#15171c';
  g.fillRect(0, 0, w, h);

  // lanes
  tracks.forEach((t, i) => {
    const y = RULER_H + i * ROW_H;
    g.fillStyle = i % 2 ? '#181a20' : '#1b1e24';
    g.fillRect(0, y, w, ROW_H);
    g.fillStyle = '#333845';
    g.fillRect(0, y + ROW_H - 1, w, 1);

    const recording = state.recording && state.writers.some((x) => x.track === t);
    const dur = recording ? state.recFrames / engine.sampleRate : (t.buffer ? t.buffer.duration : 0);
    if (!dur) return;
    const x0 = -t0 * pps;
    const x1 = (dur - t0) * pps;
    if (x1 < 0 || x0 > w) return;
    const cx0 = Math.max(0, x0), cx1 = Math.min(w, x1);
    g.fillStyle = recording ? 'rgba(255,71,87,0.18)' : 'rgba(79,163,255,0.16)';
    g.fillRect(cx0, y + 3, cx1 - cx0, ROW_H - 7);
    if (state.selected === i && !recording) {
      g.strokeStyle = '#e6e8ee';
      g.lineWidth = 2;
      g.strokeRect(x0 + 1, y + 4, x1 - x0 - 2, ROW_H - 9);
    }

    // waveform
    const mid = y + ROW_H / 2 - 1;
    const amp = ROW_H / 2 - 6;
    g.fillStyle = recording ? '#ff8a95' : '#7fbcff';
    const peaks = t.peaks;
    const rate = t.peakRate;
    for (let x = Math.floor(cx0); x < cx1; x++) {
      const a = Math.floor((t0 + x / pps) * rate);
      const b = Math.max(a + 1, Math.floor((t0 + (x + 1) / pps) * rate));
      let m = 0;
      for (let k = a; k < b && k < peaks.length; k++) if (peaks[k] > m) m = peaks[k];
      const ph = Math.max(0.5, m * amp);
      g.fillRect(x, mid - ph, 1, ph * 2);
    }
    if (t.fileName && x0 + 4 < w) {
      g.fillStyle = 'rgba(230,232,238,0.75)';
      g.font = '11px system-ui, sans-serif';
      g.fillText(t.fileName, Math.max(4, x0 + 6), y + 15);
    }
  });

  // ruler
  g.fillStyle = '#1d2027';
  g.fillRect(0, 0, w, RULER_H);
  g.fillStyle = '#333845';
  g.fillRect(0, RULER_H - 1, w, 1);
  const step = niceInterval(pps);
  g.fillStyle = '#8b92a3';
  g.font = '11px system-ui, sans-serif';
  for (let s = Math.floor(t0 / step) * step; s < t0 + w / pps; s += step) {
    const x = (s - t0) * pps;
    g.fillRect(x, RULER_H - 10, 1, 9);
    g.fillText(formatTime(s, step < 1), x + 3, 14);
  }

  // playhead
  const px = (pos - t0) * pps;
  if (px >= 0 && px <= w) {
    g.fillStyle = state.recording ? '#ff4757' : '#e6e8ee';
    g.fillRect(px, 0, 1.5, h);
  }
}

function formatTime(sec, ms = false) {
  const m = Math.floor(sec / 60);
  const s = sec - m * 60;
  const ss = ms ? s.toFixed(2).padStart(5, '0') : String(Math.floor(s)).padStart(2, '0');
  return `${m}:${ss}`;
}

function clockText(sec) {
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = Math.floor(sec % 60);
  const ms = Math.floor((sec % 1) * 1000);
  const p = (n, l = 2) => String(n).padStart(l, '0');
  return `${p(h)}:${p(m)}:${p(s)}.${p(ms, 3)}`;
}

// ---------- animation loop ----------

function frame() {
  let pos = state.playhead;
  if (state.recording) pos = state.recFrames / (engine.sampleRate || 48000);
  else if (engine.playing) {
    pos = engine.playPosition;
    if (pos > totalSeconds()) { stopPlayback(); state.playhead = 0; pos = 0; }
  }
  if (!drawTimecode()) $('clock').textContent = clockText(pos);
  else if (state.recording) {
    // The clock shows timecode, so show the take's length here.
    const txt = `${state.recInfo} · ${formatTime(pos)}`;
    if ($('take-info').textContent !== txt) $('take-info').textContent = txt;
  }

  if (state.recording || engine.playing) {
    const tl = $('timeline');
    const px = pos * state.pxPerSec - tl.scrollLeft;
    if (px > tl.clientWidth - 40) tl.scrollLeft = pos * state.pxPerSec - 40;
  }
  if (state.dirty || state.recording || engine.playing) {
    drawTimeline(pos);
    state.dirty = false;
  }
  drawMeters();
  requestAnimationFrame(frame);
}

// ---------- start-up ----------

function init() {
  loadSaved();
  tc.load();
  fillTimecodeMenu();
  tc.apply().then(fillTimecodeMenu);
  buildTrackHeaders();
  live = new LiveMixer(engine, tracks, { onPluginReady: onWamReady });
  buildStrips();
  bindControls();
  bindLive();
  bindPluginPanel();
  setMode(loadMode());
  engine.addEventListener('opened', () => net.attach(engine.input));
  net.addEventListener('change', onNetworkChange);
  bindNetworkPanel(net, {
    numTracks: MAX_CHANNELS,
    deviceChannels: () => (state.open ? engine.deviceChannels : 0),
    audioInfo: () => (state.open ? `${$('sel-input').selectedOptions[0]?.textContent || 'input'} · ${$('device-info').textContent}` : 'audio not enabled'),
  });
  onNetworkChange();
  if (net.subs.length) net.connect(); // otherwise when the AES67 window opens
  requestAnimationFrame(frame);
  Promise.all([bundledWams(), serverWams()]).then(([a, b]) => { state.serverWams = [...a, ...b]; if (state.serverWams.length) refreshAddMenus(); });
  setInterval(() => live.saveWamStates(), 5000); // third-party editors don't report changes

  if (!canUseFolders) {
    $('btn-folder').disabled = true;
    $('folder-info').textContent = 'Saving to a folder needs Chrome or Edge. Use “Download WAVs” after recording.';
  }
}

init();

// Expose for debugging and automated tests.
window.daw = { engine, state, tracks, live, tc, net };
