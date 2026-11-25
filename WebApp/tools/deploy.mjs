#!/usr/bin/env node
/*
 * Publish the app: build dist/ and push it as the only commit of the public
 * app repo, which GitHub Pages serves. The main TimeTaker repo stays
 * private; the app repo holds nothing but the current build.
 *
 *   npm run deploy                       → https://github.com/SteinerFabian0/timetaker-app.git
 *   npm run deploy -- <git remote URL>   → another repo (or a local bare repo, for a test)
 *   TT_DEPLOY_REMOTE=<url> npm run deploy
 *
 * Each run replaces the repo's history with one commit (force push), so old
 * builds do not pile up. Installed apps pick the new build up the next time
 * they are opened with internet (the service worker, tools/build.mjs).
 *
 * One-time GitHub setup: create the empty public repo, then after the first
 * deploy: Settings → Pages → Deploy from a branch → main, / (root).
 * The site: https://steinerfabian0.github.io/timetaker-app/
 */

import { execFileSync } from 'node:child_process';
import { writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(fileURLToPath(new URL('..', import.meta.url)));
const DIST = join(ROOT, 'dist');
const REMOTE = process.argv[2] ?? process.env.TT_DEPLOY_REMOTE ?? 'https://github.com/SteinerFabian0/timetaker-app.git';

const run = (cmd, args, opts = {}) => execFileSync(cmd, args, { stdio: 'inherit', ...opts });
const out = (cmd, args, cwd) => execFileSync(cmd, args, { cwd, encoding: 'utf8' }).trim();

// The source commit the build comes from, for the deploy commit's message.
const rev = out('git', ['rev-parse', '--short', 'HEAD'], ROOT);
const dirty = out('git', ['status', '--porcelain', '--', '.'], ROOT) !== '';

run(process.execPath, [join(ROOT, 'tools', 'build.mjs')]);          // wipes and rewrites dist/
await writeFile(join(DIST, '.nojekyll'), '');                      // serve every file as it is (no Jekyll)

const git = (...args) => run('git', args, { cwd: DIST });
git('init', '-q', '-b', 'main');
git('add', '-A');
git('commit', '-q', '-m', `TimeTaker app, built from ${rev}${dirty ? ' (with uncommitted changes)' : ''}`);
console.log(`pushing to ${REMOTE} …`);
git('push', '--force', REMOTE, 'main:main');
console.log('done. GitHub Pages publishes it within a minute or two.');
