// The AES67 window: pick streams to receive and the tracks they land on.

import { LATENCY_CHOICES } from './aes67.js';
import { parseSdpText } from './aes67-sdp.js';

const $ = (id) => document.getElementById(id);

export function bindNetworkPanel(net, { numTracks, audioInfo, deviceChannels }) {
  // New streams go on the first free tracks after the audio interface's channels.
  const freeStart = (channels) => net.freeStart(channels, Math.min(deviceChannels(), numTracks - channels));
  const dlg = $('aes67-dialog');
  let shape = ''; // which rows are on screen, to rebuild only when that changes
  const rows = new Map(); // key -> { status, start }

  $('btn-aes67').addEventListener('click', () => {
    if (!net.ws) net.connect();
    shape = '';
    render();
    if (dlg.showModal) dlg.showModal(); else dlg.setAttribute('open', '');
  });
  $('aes-close').addEventListener('click', () => dlg.close());

  const lat = $('aes-latency');
  for (const ms of LATENCY_CHOICES) {
    const o = document.createElement('option');
    o.value = ms;
    o.textContent = `${ms} ms`;
    lat.appendChild(o);
  }
  lat.addEventListener('change', () => net.setLatency(+lat.value));
  $('aes-iface').addEventListener('change', (e) => net.setInterface(e.target.value));

  $('aes-report').addEventListener('click', async () => {
    const text = net.report(audioInfo());
    try {
      await navigator.clipboard.writeText(text);
      $('aes-report').textContent = 'Copied';
    } catch (_) {
      window.prompt('Copy this report:', text);
    }
    net.resetStats();
    setTimeout(() => { $('aes-report').textContent = 'Copy report'; }, 1500);
  });

  const msg = $('aes-sdp-msg');
  const addSdp = (text) => {
    const r = parseSdpText(text);
    if (!r.ok) { msg.textContent = r.why; return; }
    if (net.subscription(r.info.key)) { msg.textContent = `${r.info.name} is already on.`; return; }
    net.add(text, r.info, freeStart(r.info.channels));
    msg.textContent = `Added ${r.info.name}.`;
    $('aes-sdp-text').value = '';
  };
  $('aes-sdp-add').addEventListener('click', () => addSdp($('aes-sdp-text').value));
  $('aes-sdp-file').addEventListener('click', () => $('aes-sdp-input').click());
  $('aes-sdp-input').addEventListener('change', async (e) => {
    const f = e.target.files[0];
    e.target.value = '';
    if (f) addSdp(await f.text());
  });

  net.addEventListener('change', () => { if (dlg.open) render(); });

  function render() {
    const st = $('aes-state');
    if (net.available) {
      st.textContent = `Receiver connected · ${net.discovered.length} stream${net.discovered.length === 1 ? '' : 's'} announced on the network`;
      st.className = 'aes-state ok';
    } else if (net.tried) {
      st.textContent = 'The AES67 receiver isn\'t reachable. It runs inside the Docker container, so open the app from the container (not a plain web server) and check the container is running.';
      st.className = 'aes-state error';
    } else {
      st.textContent = 'Connecting to the receiver…';
      st.className = 'aes-state';
    }

    const sel = $('aes-iface');
    const ifs = net.interfaces.map((i) => `${i.ip}|${i.name}`).join(',');
    if (sel.dataset.list !== ifs) {
      sel.dataset.list = ifs;
      sel.innerHTML = '';
      for (const i of net.interfaces) {
        const o = document.createElement('option');
        o.value = i.ip;
        o.textContent = `${i.name}  ${i.ip}`;
        sel.appendChild(o);
      }
      if (!net.interfaces.length) sel.appendChild(new Option('—', ''));
    }
    sel.value = net.iface;
    sel.disabled = !net.available;
    lat.value = net.latencyMs;

    // Announced streams, plus streams added by hand that aren't announced.
    const list = net.discovered.map((info) => ({ info, sdp: info.sdp }));
    for (const s of net.subs) {
      if (s.info && !list.some((x) => x.info.key === s.info.key)) list.push({ info: s.info, sdp: s.sdp });
    }
    const subsShape = net.subs.map((s) => `${s.info && s.info.key}@${s.start}`).join(',');
    const newShape = `${list.map((x) => x.info.key + x.info.format + x.info.name).join(',')}|${subsShape}`;
    if (newShape !== shape) {
      shape = newShape;
      build(list);
    }
    for (const [key, r] of rows) {
      const s = net.subscription(key);
      r.status.textContent = s ? net.status(s) : 'Not received';
      r.status.className = `aes-status${s ? (s.error || (s.server && !s.server.receiving) ? ' bad' : s.server ? ' ok' : '') : ''}`;
    }
  }

  function build(list) {
    const box = $('aes-list');
    box.innerHTML = '';
    rows.clear();
    if (!list.length) {
      const e = document.createElement('div');
      e.className = 'aes-empty';
      e.textContent = net.available
        ? 'No streams announced yet. Senders announce every 30 seconds or so; or add one from its SDP below.'
        : 'No streams yet.';
      box.appendChild(e);
      return;
    }
    for (const { info, sdp } of list) {
      const s = net.subscription(info.key);
      const row = document.createElement('div');
      row.className = 'aes-stream';
      const on = document.createElement('input');
      on.type = 'checkbox';
      on.checked = !!s;
      on.title = 'Receive this stream';
      on.addEventListener('change', () => {
        const cur = net.subscription(info.key);
        if (on.checked && !cur) net.add(sdp, info, freeStart(info.channels));
        else if (!on.checked && cur) net.remove(cur.id);
      });
      const name = document.createElement('span');
      name.className = 'aes-name';
      name.textContent = info.name;

      const where = document.createElement('label');
      where.textContent = 'Tracks from';
      const start = document.createElement('select');
      for (let t = 0; t < numTracks; t++) {
        const end = Math.min(numTracks, t + info.channels);
        start.appendChild(new Option(end - t > 1 ? `${t + 1}-${end}` : `${t + 1}`, t));
      }
      start.value = s ? s.start : freeStart(info.channels);
      start.disabled = !s;
      start.addEventListener('change', () => {
        const cur = net.subscription(info.key);
        if (cur) net.move(cur.id, +start.value);
      });
      where.appendChild(start);

      const format = document.createElement('span');
      format.className = 'aes-format';
      format.textContent = `${info.format} · ${info.address}:${info.port}${info.origin ? ` · from ${info.origin}` : ''}`;
      const status = document.createElement('span');
      status.className = 'aes-status';
      row.append(on, name, where, format, status);
      box.appendChild(row);
      rows.set(info.key, { status, start });
    }
  }
}
