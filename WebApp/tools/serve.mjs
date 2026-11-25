#!/usr/bin/env node
/*
 * Dev server for the web app: static files from WebApp/, and .ts files served
 * as JavaScript with their types stripped by Node itself
 * (module.stripTypeScriptTypes). No bundler, no install. The same "run .ts
 * directly" rule as the tests.
 *
 *   npm run app              → http://localhost:5173/app/    (the GUI)
 *   npm run debug            → http://localhost:5173/debug/  (debug console)
 *
 *   node tools/serve.mjs --dist → the production build in dist/ (npm run build),
 *                                  on port 5180 (its service worker stays off the dev origin)
 *
 * Binds to 127.0.0.1 only: Web Serial and Web Bluetooth need a secure context,
 * and localhost is one. Not a production server: the app ships as the static
 * PWA tools/build.mjs writes.
 */

import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, normalize, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { stripTypeScriptTypes } from 'node:module';

// --dist: serve the production build (npm run build) instead of the sources.
const ROOT = resolve(fileURLToPath(new URL(process.argv.includes('--dist') ? '../dist' : '..', import.meta.url)));
// The build gets its own port: its service worker must never sit on the dev server's origin.
const PORT = Number(process.env.PORT ?? (process.argv.includes('--dist') ? 5180 : 5173));
const HOST = '127.0.0.1';

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.ts': 'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.webmanifest': 'application/manifest+json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
  '.woff2': 'font/woff2',
};

process.removeAllListeners('warning');   // stripTypeScriptTypes is "experimental": known, fine

const server = createServer(async (req, res) => {
  try {
    const url = new URL(req.url ?? '/', `http://${HOST}`);
    if (url.pathname === '/') {                       // the GUI is the default page
      res.writeHead(302, { location: '/app/' }).end();
      return;
    }
    if (url.pathname === '/favicon.ico') {            // browsers ask for it regardless of <link rel=icon>
      res.writeHead(302, { location: '/app/assets/logo-mark.svg' }).end();
      return;
    }
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
  console.log(`TimeTaker web app ${process.argv.includes('--dist') ? 'build (dist/)' : 'dev server'}: http://localhost:${PORT}/app/  (debug console: /debug/)`);
});
