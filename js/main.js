import { AudioEngine, MAX_CHANNELS } from './audio-engine.js';
import { peaksOf } from './wav.js';
import { DiskWriter, MemoryWriter, canUseFolders, pickFolder, downloadBlob, safeName } from './storage.js';
import { namesFromCsv, namesToCsv } from './csv.js';

const PEAK_STEP = 1024; // samples per waveform peak
const ROW_H = 64;
const RULER_H = 28;
const STORE_KEY = 'daw2110.tracks';

const $ = (id) => document.getElementById(id);
const engine = new AudioEngine();

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

loadSaved();
buildTrackHeaders();
bindControls();
requestAnimationFrame(frame);

if (!canUseFolders) {
  $('btn-folder').disabled = true;
  $('folder-info').textContent = 'Saving to a folder needs Chrome or Edge. Use “Download WAVs” after recording.';
}

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
  t.el.title = t.hasInput ? '' : 'This input device has no channel for this track';
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
    $('device-info').textContent = `${info.channels} ch · ${(info.sampleRate / 1000).toFixed(1)} kHz` +
      (info.channels < MAX_CHANNELS ? ` · tracks ${info.channels + 1}–${MAX_CHANNELS} have no input` : '');
    tracks.forEach((t) => (t.hasInput = t.index < info.channels));
    refreshAll();
    if (info.channels < MAX_CHANNELS) {
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
  try {
    state.writers = await Promise.all(armed.map(async (t) => {
      const fileName = `${tag}_${String(t.index + 1).padStart(2, '0')}_${safeName(t.name)}.wav`;
      const writer = state.folder
        ? await DiskWriter.create(state.folder, fileName, sr)
        : new MemoryWriter(fileName, sr);
      return { track: t, writer, fileName };
    }));
  } catch (err) {
    notice(`Could not create files in the save folder: ${err.message}`, true);
    return;
  }

  state.take = take;
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
  $('take-info').textContent = `Recording take ${take} · ${armed.length} track${armed.length === 1 ? '' : 's'}`;
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
    (state.folder && !memory ? ` · saved to “${state.folder.name}”` : '');
  if (problems.length) notice(`Some tracks failed to save: ${problems.join('; ')}`, true);
  state.dirty = true;
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
  for (const t of tracks) {
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
  $('clock').textContent = clockText(pos);

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

// Expose for debugging and automated tests.
window.daw = { engine, state, tracks };
