// Reading AES67 / ST 2110-30 SDP stream descriptions. Used by the page (to add a
// stream by hand) and by the receiver in the Docker container (server/).
// Same rules as the Mac app (mac/Source/Aes67.cpp).

const DOT = ' · ';

// Reads an SDP description. Returns { ok: true, info } or { ok: false, why }.
// We take uncompressed L16/L24 audio over RTP, the first audio stream only.
export function parseSdp(text) {
  const s = {
    name: '', sessionId: '', origin: '', address: '', sourceFilter: '',
    port: 5004, payloadType: 96, channels: 0, bits: 24, rate: 48000, ptimeMs: 1, sdp: String(text || ''),
  };
  let rtpmap = '', sessionConn = '', mediaConn = '';
  let inMedia = false, haveAudio = false;
  for (const raw of s.sdp.split(/\r?\n/)) {
    const line = raw.trim();
    if (line.length < 2 || line[1] !== '=') continue;
    const v = line.slice(2).trim();
    const f = v.split(/\s+/).filter(Boolean);
    switch (line[0]) {
      case 'o':
        if (f.length >= 6) { s.sessionId = f[1]; s.origin = f[5]; }
        break;
      case 's':
        if (!inMedia) s.name = v;
        break;
      case 'c':
        if (f.length >= 3) {
          const addr = f[2].split('/')[0];
          if (inMedia) mediaConn = addr; else sessionConn = addr;
        }
        break;
      case 'm':
        if (haveAudio) { inMedia = false; break; } // only the first audio stream
        inMedia = f[0] === 'audio';
        if (inMedia && f.length >= 4) {
          haveAudio = true;
          s.port = parseInt(f[1], 10) || 0;
          s.payloadType = parseInt(f[3], 10) || 0;
        }
        break;
      case 'a':
        if (!inMedia && haveAudio) break;
        if (v.startsWith('rtpmap:')) {
          const pt = parseInt(v.slice(7), 10);
          if (pt === s.payloadType || !haveAudio) rtpmap = v.slice(v.indexOf(' ') + 1).trim();
        } else if (v.startsWith('ptime:')) {
          s.ptimeMs = parseFloat(v.slice(6)) || 0;
        } else if (v.startsWith('source-filter:')) {
          const g = v.slice(14).trim().split(/\s+/);
          if (g.length >= 5) s.sourceFilter = g[g.length - 1];
        }
        break;
      default:
    }
  }
  s.address = mediaConn || sessionConn;
  if (!haveAudio) return { ok: false, why: 'No audio stream in the SDP.' };
  if (!s.address) return { ok: false, why: 'The SDP has no address (c= line).' };
  const r = rtpmap.split('/');
  const enc = (r[0] || '').toUpperCase();
  if (enc !== 'L24' && enc !== 'L16') {
    return { ok: false, why: enc ? `${enc} isn't supported; AES67 uses L24 or L16.` : "The SDP doesn't say the audio format." };
  }
  s.bits = enc === 'L24' ? 24 : 16;
  s.rate = parseFloat(r[1]) || 0;
  s.channels = r.length > 2 ? parseInt(r[2], 10) || 0 : 1;
  if (s.rate < 8000 || s.channels < 1 || s.channels > 64) return { ok: false, why: 'Unusual rate or channel count in the SDP.' };
  if (!(s.ptimeMs > 0)) s.ptimeMs = 1;
  if (!s.port) return { ok: false, why: 'The SDP has no port.' };
  if (!s.name || s.name === '-') s.name = s.address;
  return { ok: true, info: s };
}

// One stream, whatever announced it.
export const streamKey = (s) => `${s.origin}/${s.sessionId}/${s.address}:${s.port}`;

// "8 ch · L24 · 48 kHz · 1 ms"
export function formatStream(s) {
  const packet = s.ptimeMs >= 1
    ? `${Number.isInteger(s.ptimeMs) ? s.ptimeMs : s.ptimeMs.toFixed(2)} ms`
    : `${Math.round(s.ptimeMs * 1000)} µs`;
  const khz = s.rate % 1000 === 0 ? String(s.rate / 1000) : (s.rate / 1000).toFixed(1);
  return `${s.channels} ch${DOT}L${s.bits}${DOT}${khz} kHz${DOT}${packet}`;
}

// parseSdp plus the stream's key and format line, as the page shows them.
export function parseSdpText(text) {
  const r = parseSdp(text);
  if (r.ok) Object.assign(r.info, { key: streamKey(r.info), format: formatStream(r.info) });
  return r;
}

// True when every word of `query` is in the stream's name, address, sender or format.
export function matchesSearch(info, query) {
  const words = String(query || '').toLowerCase().split(/\s+/).filter(Boolean);
  if (!words.length) return true;
  const text = `${info.name} ${info.address}:${info.port} ${info.origin} ${info.format || formatStream(info)}`.toLowerCase();
  return words.every((w) => text.includes(w));
}
