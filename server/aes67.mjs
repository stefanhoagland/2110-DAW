// AES67 / SMPTE ST 2110-30 helpers for the receiver service: SDP (shared with the
// page), SAP announcements and RTP packets. Same rules as the Mac app (mac/Source/Aes67.cpp).

export { parseSdp, streamKey, formatStream } from '../js/aes67-sdp.js';

// Reads a SAP packet (RFC 2974). Returns { sdp, deleted } or null if it isn't SAP with SDP.
export function parseSap(buf) {
  const p = buf;
  if (p.length < 8 || (p[0] >> 5) !== 1) return null;
  if (p[0] & 0x03) return null; // encrypted or compressed
  const deleted = (p[0] & 0x04) !== 0;
  let at = 4 + ((p[0] & 0x10) ? 16 : 4) + p[1] * 4;
  if (at >= p.length) return null;
  let rest = p.subarray(at).toString('utf8');
  if (!rest.startsWith('v=0')) {
    // An optional payload type ("application/sdp") ending in a zero byte.
    let z = at;
    while (z < p.length && p[z] !== 0) z++;
    if (p.subarray(at, z).toString('utf8') !== 'application/sdp') return null;
    at = z + 1;
    rest = p.subarray(at).toString('utf8');
  }
  return rest.startsWith('v=0') ? { sdp: rest, deleted } : null;
}

// Unpacks an RTP packet. Returns { seq, timestamp, payloadType, start, end } (the
// payload's byte range in buf) or null when it isn't RTP version 2.
export function parseRtp(buf) {
  const b = buf;
  if (b.length < 12 || (b[0] >> 6) !== 2) return null;
  let start = 12 + 4 * (b[0] & 15);
  if ((b[0] & 0x10) && start + 4 <= b.length) start += 4 + 4 * ((b[start + 2] << 8) | b[start + 3]);
  let end = b.length;
  if (b[0] & 0x20) end -= b[b.length - 1];
  if (end <= start) return null;
  return {
    seq: (b[2] << 8) | b[3],
    timestamp: b.readUInt32BE(4),
    payloadType: b[1] & 0x7f,
    start,
    end,
  };
}
