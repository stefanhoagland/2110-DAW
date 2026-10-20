// Third-party plugins in the Web Audio Modules 2.0 format (webaudiomodules.com).
// A WAM is an ES module whose default export has createInstance(groupId, ctx, state).

import { initializeWamHost } from './vendor/wam-sdk.js';

const KNOWN_KEY = 'daw2110.wams';
const hosts = new WeakMap(); // AudioContext -> Promise<hostGroupId>
const modules = new Map(); // url -> Promise<WAM class>

function hostGroup(ctx) {
  if (!hosts.has(ctx)) hosts.set(ctx, initializeWamHost(ctx).then(([id]) => id));
  return hosts.get(ctx);
}

const absolute = (url) => new URL(url, document.baseURI).href;

export function loadWamModule(url) {
  if (!modules.has(url)) {
    const p = import(absolute(url)).then((m) => {
      const W = m.default;
      if (!W || typeof W.createInstance !== 'function') throw new Error('That file is not a Web Audio Module.');
      return W;
    });
    p.catch(() => modules.delete(url));
    modules.set(url, p);
  }
  return modules.get(url);
}

// A name to show before the plugin has told us its own.
export function nameFromUrl(url) {
  const parts = url.split(/[?#]/)[0].split('/').filter(Boolean);
  const file = parts.pop() || 'Plugin';
  const base = /^index\.m?js$/i.test(file) && parts.length ? parts.pop() : file.replace(/\.m?js$/i, '');
  return decodeURIComponent(base) || 'Plugin';
}

// Wraps a WAM in an insert slot. Audio passes straight through until the
// plugin has loaded (or if it fails to load), so the chain is never broken.
export function createWamInsert(ctx, pl, onReady) {
  const input = ctx.createGain();
  const output = ctx.createGain();
  input.connect(output);
  const inst = { input, output, set() {}, wam: null, error: '' };
  inst.ready = (async () => {
    const W = await loadWamModule(pl.url);
    const id = await hostGroup(ctx);
    const wam = await W.createInstance(id, ctx, pl.state || undefined);
    if (inst.disposed) { wam.audioNode.destroy?.(); return; }
    input.disconnect();
    input.connect(wam.audioNode);
    wam.audioNode.connect(output);
    inst.wam = wam;
    const name = wam.descriptor && wam.descriptor.name;
    if (name) pl.name = name;
  })().catch((e) => { inst.error = e && e.message ? e.message : String(e); })
    .finally(() => onReady && onReady(inst));
  inst.dispose = () => {
    inst.disposed = true;
    output.disconnect();
    if (inst.wam) { inst.wam.audioNode.disconnect(); inst.wam.audioNode.destroy?.(); }
  };
  return inst;
}

// Plugins the user has loaded before, so they show up in the + Plugin menu.
export function knownWams() {
  try {
    const list = JSON.parse(localStorage.getItem(KNOWN_KEY) || '[]');
    return Array.isArray(list) ? list.filter((x) => x && typeof x.url === 'string') : [];
  } catch (_) { return []; }
}

export function rememberWam(url, name) {
  const list = knownWams().filter((x) => x.url !== url);
  list.push({ url, name: name || nameFromUrl(url) });
  list.sort((a, b) => a.name.localeCompare(b.name));
  try { localStorage.setItem(KNOWN_KEY, JSON.stringify(list)); } catch (_) { /* storage unavailable */ }
}

export function forgetWam(url) {
  try { localStorage.setItem(KNOWN_KEY, JSON.stringify(knownWams().filter((x) => x.url !== url))); } catch (_) { /* storage unavailable */ }
}

// Plugins in the server's plugins folder (Docker: /config/plugins), one
// subfolder per plugin with an index.js. Empty when the server has none.
export async function serverWams() {
  try {
    const res = await fetch('plugins/', { cache: 'no-store' });
    if (!res.ok || !(res.headers.get('content-type') || '').includes('json')) return [];
    const list = await res.json();
    return list
      .filter((e) => e.type === 'directory')
      .map((e) => ({ url: `plugins/${encodeURIComponent(e.name)}/index.js`, name: e.name }));
  } catch (_) { return []; }
}
