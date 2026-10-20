// Channel-name lists as CSV: "Channel,Name" with one row per channel.

export function parseCsv(text) {
  const rows = [];
  let row = [], field = '', quoted = false;
  text = text.replace(/^﻿/, '');
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (quoted) {
      if (c === '"' && text[i + 1] === '"') { field += '"'; i++; }
      else if (c === '"') quoted = false;
      else field += c;
    } else if (c === '"') quoted = true;
    else if (c === ',' || c === ';' || c === '\t') { row.push(field); field = ''; }
    else if (c === '\n' || c === '\r') {
      if (c === '\r' && text[i + 1] === '\n') i++;
      row.push(field); rows.push(row); row = []; field = '';
    } else field += c;
  }
  if (field !== '' || row.length) { row.push(field); rows.push(row); }
  return rows.map((r) => r.map((f) => f.trim())).filter((r) => r.some((f) => f !== ''));
}

// Returns [{ channel (1-based), name }]. Accepts "Channel,Name" rows (any
// order, header optional) or a plain list of names, one per line.
export function namesFromCsv(text) {
  let rows = parseCsv(text);
  if (!rows.length) return [];
  let chCol = -1, nameCol = -1;
  const head = rows[0].map((h) => h.toLowerCase());
  const hasHeader = head.some((h) => /^(ch|channel|input|#|no\.?|number|name|track|label|source)$/.test(h));
  if (hasHeader) {
    chCol = head.findIndex((h) => /^(ch|channel|input|#|no\.?|number)$/.test(h));
    nameCol = head.findIndex((h) => /^(name|track|label|source)$/.test(h));
    rows = rows.slice(1);
  } else if (rows.every((r) => r.length >= 2 && /^\d+$/.test(r[0]))) {
    chCol = 0; nameCol = 1;
  }
  if (nameCol < 0) nameCol = chCol === 0 ? 1 : 0;
  return rows
    .map((r, i) => ({ channel: chCol >= 0 ? parseInt(r[chCol], 10) : i + 1, name: r[nameCol] || '' }))
    .filter((x) => Number.isInteger(x.channel) && x.channel > 0 && x.name);
}

function quote(s) {
  return /[",\n\r;]/.test(s) || s !== s.trim() ? `"${s.replace(/"/g, '""')}"` : s;
}

export function namesToCsv(names) {
  return ['Channel,Name', ...names.map((n, i) => `${i + 1},${quote(n)}`)].join('\r\n') + '\r\n';
}
