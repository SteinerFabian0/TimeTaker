#!/usr/bin/env node
/*
 * Dev server for the web app: static files from WebApp/, and .ts files served
 * as JavaScript with their types stripped by Node itself
 * (module.stripTypeScriptTypes). No bundler, no install. The same "run .ts
 * directly" rule as the tests.
 *
 *   npm run debug            → http://localhost:5173/debug/
 *
 * Binds to 127.0.0.1 only: Web Serial and Web Bluetooth need a secure context,
 * and localhost is one. Not a production server: the app ships as a static
 * PWA later (SYSTEM_ARCHITECTURE §11).
 */

import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, normalize, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { stripTypeScriptTypes } from 'node:module';

const ROOT = resolve(fileURLToPath(new URL('..', import.meta.url)));
const PORT = Number(process.env.PORT ?? 5173);
const HOST = '127.0.0.1';

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.ts': 'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
};

process.removeAllListeners('warning');   // stripTypeScriptTypes is "experimental": known, fine

const server = createServer(async (req, res) => {
  try {
    const url = new URL(req.url ?? '/', `http://${HOST}`);
    let path = normalize(decodeURIComponent(url.pathname));
    if (path.endsWith('/')) path += 'index.html';
    const file = join(ROOT, path);
    if (file !== ROOT && !file.startsWith(ROOT + sep)) {
      res.writeHead(403).end('forbidden');
      return;
    }
    const info = await stat(file).catch(() => null);
    if (!info?.isFile()) {
      res.writeHead(404, { 'content-type': 'text/plain' }).end(`not found: ${url.pathname}`);
      return;
    }

    const ext = extname(file);
    let body = await readFile(file);
    if (ext === '.ts') {
      body = Buffer.from(stripTypeScriptTypes(body.toString('utf8'), { mode: 'strip' }));
    }
    res.writeHead(200, { 'content-type': TYPES[ext] ?? 'application/octet-stream', 'cache-control': 'no-store' });
    res.end(body);
  } catch (err) {
    res.writeHead(500, { 'content-type': 'text/plain' }).end(String(err));
  }
});

server.listen(PORT, HOST, () => {
  console.log(`TimeTaker web app dev server: http://localhost:${PORT}/debug/`);
});
