// Minimal static file server for local development: `npm start`, then open http://localhost:8110
// With { aes67: true } (as `npm start` does) it also runs the AES67 receiver at /aes67/ws.
import http from 'node:http';
import { readFile, readdir } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('..', import.meta.url));
const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.wav': 'audio/wav', '.json': 'application/json' };

export async function serve(port = 8110, { aes67 = false } = {}) {
  const server = http.createServer(async (req, res) => {
    const path = normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)).replace(/^([/\\])+/, '');
    if (path.startsWith('..')) { res.writeHead(403).end(); return; }
    if (path === 'plugins' || path === 'plugins/' || path === 'plugins\\') {
      // Same JSON folder listing the Docker image's nginx gives for /plugins/.
      const dirs = await readdir(join(root, 'plugins'), { withFileTypes: true }).catch(() => []);
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(JSON.stringify(dirs.filter((d) => d.isDirectory()).map((d) => ({ name: d.name, type: 'directory' }))));
      return;
    }
    if (path === 'bundled-plugins/index.json' || path === 'bundled-plugins\\index.json') {
      // Only there after `npm run plugins`; otherwise no bundled plugins.
      const body = await readFile(join(root, path)).catch(() => '[]');
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(body);
      return;
    }
    try {
      const body = await readFile(join(root, path || 'index.html'));
      res.writeHead(200, { 'content-type': types[extname(path || 'index.html')] || 'application/octet-stream' });
      res.end(body);
    } catch {
      res.writeHead(404).end('not found');
    }
  });
  if (aes67) {
    const { Aes67Service } = await import('../server/aes67-server.mjs');
    const service = new Aes67Service();
    server.aes67 = service;
    server.on('upgrade', (req, socket) => {
      if (req.url.startsWith('/aes67/ws')) service.handleUpgrade(req, socket);
      else socket.destroy();
    });
    server.on('close', () => service.close());
  }
  return new Promise((resolve) => server.listen(port, () => resolve(server)));
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const port = Number(process.env.PORT) || 8110;
  await serve(port, { aes67: true });
  console.log(`2110 DAW running at http://localhost:${port}`);
}
