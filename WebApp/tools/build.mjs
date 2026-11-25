#!/usr/bin/env node
/*
 * Production build of the GUI: WebApp/dist/, a static, offline-capable site
 * that any HTTPS host (or localhost) can serve. No bundler, no install:
 *
 *   - Walks the module graph from app/main.ts; every reachable .ts file is
 *     written as .js with its types stripped by Node itself (the same
 *     module.stripTypeScriptTypes the dev server uses) and its relative
 *     ".ts" imports rewritten to ".js". Same folder layout as the source
 *     (dist/app, dist/src, dist/vendor), so every relative path still holds.
 *   - Copies index.html (main.ts → main.js, plus the service worker
 *     registration), styles.css, the manifest, app/assets/ and vendor/.
 *   - Writes dist/app/sw.js: precaches every file of this build under a cache
 *     named after a hash of their contents, serves them cache-first (so the
 *     app starts with no network), and drops older builds' caches.
 *   - dist/index.html sends the site root to app/.
 *
 *   npm run build            → dist/
 *   npm run serve:dist       → http://localhost:5180/app/  (the build, as hosted)
 *
 * Deploy: copy dist/ to the web root (or a sub-path: every URL is relative).
 */

import { createHash } from 'node:crypto';
import { cp, mkdir, readFile, readdir, rm, stat, writeFile } from 'node:fs/promises';
import { stripTypeScriptTypes } from 'node:module';
import { dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

process.removeAllListeners('warning');   // stripTypeScriptTypes is "experimental": known, fine

const ROOT = resolve(fileURLToPath(new URL('..', import.meta.url)));
const OUT = join(ROOT, 'dist');
const ENTRY = join(ROOT, 'app', 'main.ts');

/** Import specifiers of a module: static `from '…'`, side-effect `import '…'`, `import('…')`, `export … from '…'`. */
export function specifiers(js) {
  const out = new Set();
  const re = /(?:\bfrom\s*|\bimport\s*\(?\s*)(['"])([^'"\n]+)\1/g;
  for (let m; (m = re.exec(js));) out.add(m[2]);
  return [...out];
}

/** Relative ".ts" specifiers → ".js" (bare ones are left to the import map). */
export function rewriteImports(js) {
  return js.replace(/(\bfrom\s*|\bimport\s*\(?\s*)(['"])(\.{1,2}\/[^'"\n]+)\.ts\2/g, '$1$2$3.js$2');
}

async function build() {
  await rm(OUT, { recursive: true, force: true });

  // ---- the module graph from app/main.ts
  const seen = new Set();
  const queue = [ENTRY];
  while (queue.length) {
    const file = queue.pop();
    if (seen.has(file)) continue;
    seen.add(file);
    const js = stripTypeScriptTypes(await readFile(file, 'utf8'), { mode: 'strip' });
    for (const s of specifiers(js)) {
      if (!s.startsWith('.')) continue;                  // bare: vendor/ via the import map
      const dep = resolve(dirname(file), s);
      if (!dep.startsWith(ROOT + sep)) throw new Error(`${relative(ROOT, file)} imports outside WebApp: ${s}`);
      if (!dep.endsWith('.ts')) throw new Error(`${relative(ROOT, file)}: only .ts imports are expected, got ${s}`);
      queue.push(dep);
    }
    const target = join(OUT, relative(ROOT, file)).replace(/\.ts$/, '.js');
    await mkdir(dirname(target), { recursive: true });
    await writeFile(target, rewriteImports(js));
  }

  // ---- static files
  await cp(join(ROOT, 'vendor'), join(OUT, 'vendor'), { recursive: true });
  await cp(join(ROOT, 'app', 'assets'), join(OUT, 'app', 'assets'), { recursive: true });
  for (const f of ['styles.css', 'manifest.webmanifest']) await cp(join(ROOT, 'app', f), join(OUT, 'app', f));

  let index = await readFile(join(ROOT, 'app', 'index.html'), 'utf8');
  if (!index.includes('src="main.ts"')) throw new Error('index.html: <script src="main.ts"> not found');
  index = index.replace('src="main.ts"', 'src="main.js"').replace('</body>', `  <script>
    // Offline: the service worker keeps this build in the browser's cache.
    if ('serviceWorker' in navigator) navigator.serviceWorker.register('sw.js').catch(() => {});
  </script>
</body>`);
  await writeFile(join(OUT, 'app', 'index.html'), index);
  await writeFile(join(OUT, 'index.html'),
    '<!doctype html><meta charset="utf-8"><title>TimeTaker</title><meta http-equiv="refresh" content="0; url=app/"><a href="app/">TimeTaker</a>\n');

  // ---- the service worker: every file of the build, under a content hash
  const files = (await walk(OUT)).filter(f => f !== join(OUT, 'index.html')).sort();
  const hash = createHash('sha256');
  for (const f of files) hash.update(relative(OUT, f)).update(await readFile(f));
  const version = hash.digest('hex').slice(0, 12);
  const urls = files.map(f => relative(join(OUT, 'app'), f).split(sep).join('/'));
  await writeFile(join(OUT, 'app', 'sw.js'), serviceWorker(version, urls));

  const bytes = (await Promise.all(files.map(async f => (await stat(f)).size))).reduce((a, b) => a + b, 0);
  console.log(`dist/: ${seen.size} modules, ${files.length + 1} files, ${(bytes / 1024).toFixed(0)} kB, build ${version}`);
}

async function walk(dir) {
  const out = [];
  for (const e of await readdir(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) out.push(...await walk(p)); else out.push(p);
  }
  return out;
}

/** The service worker source (plain JS, runs in the browser). */
function serviceWorker(version, urls) {
  return `// Generated by tools/build.mjs: do not edit. Build ${version}.
// Precaches this build and serves it cache-first, so the app starts and runs
// with no network. A new build installs in the background and takes over on
// the next start of the app (the page running now keeps its modules).
const CACHE = 'timetaker-${version}';
const FILES = ${JSON.stringify(['./', ...urls], null, 0)};

self.addEventListener('install', (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(FILES)));
});

self.addEventListener('activate', (e) => {
  e.waitUntil(caches.keys().then((keys) =>
    Promise.all(keys.filter((k) => k.startsWith('timetaker-') && k !== CACHE).map((k) => caches.delete(k)))));
});

self.addEventListener('fetch', (e) => {
  const req = e.request;
  if (req.method !== 'GET' || new URL(req.url).origin !== location.origin) return;
  e.respondWith((async () => {
    const cache = await caches.open(CACHE);
    // A page load (any URL of the app) gets the cached index.html.
    const hit = await cache.match(req, { ignoreSearch: true })
      ?? (req.mode === 'navigate' ? await cache.match('./') : undefined);
    return hit ?? fetch(req);
  })());
});
`;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  build().catch((err) => { console.error(err instanceof Error ? err.message : err); process.exit(1); });
}
