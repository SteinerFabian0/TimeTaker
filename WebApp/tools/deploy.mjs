#!/usr/bin/env node
/*
 * Publish the app: build dist/ and push it as the only commit of the
 * branch gh-pages of this repo (origin), which GitHub Pages serves. main
 * keeps the sources; gh-pages holds nothing but the current build.
 *
 *   npm run deploy                       → origin, branch gh-pages
 *   npm run deploy -- <git remote URL>   → another repo (or a local bare repo, for a test)
 *   TT_DEPLOY_REMOTE=<url> npm run deploy
 *
 * Each run replaces the branch with one commit (force push), so old builds do
 * not pile up in the history. Installed apps pick the new build up the next time
 * they are opened with internet (the service worker, tools/build.mjs).
 *
 * One-time GitHub setup, after the first deploy: Settings → Pages → Deploy
 * from a branch → gh-pages, / (root). The repo must be public (free plan).
 * The site: https://steinerfabian0.github.io/TimeTaker/app/
 */

import { execFileSync } from 'node:child_process';
import { writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(fileURLToPath(new URL('..', import.meta.url)));
const DIST = join(ROOT, 'dist');
const BRANCH = 'gh-pages';

const run = (cmd, args, opts = {}) => execFileSync(cmd, args, { stdio: 'inherit', ...opts });
const out = (cmd, args, cwd) => execFileSync(cmd, args, { cwd, encoding: 'utf8' }).trim();

// The source commit the build comes from, for the deploy commit's message.
const rev = out('git', ['rev-parse', '--short', 'HEAD'], ROOT);
const dirty = out('git', ['status', '--porcelain', '--', '.'], ROOT) !== '';
const REMOTE = process.argv[2] ?? process.env.TT_DEPLOY_REMOTE ?? out('git', ['remote', 'get-url', 'origin'], ROOT);

run(process.execPath, [join(ROOT, 'tools', 'build.mjs')]);          // wipes and rewrites dist/
await writeFile(join(DIST, '.nojekyll'), '');                      // serve every file as it is (no Jekyll)

const git = (...args) => run('git', args, { cwd: DIST });
git('init', '-q', '-b', 'main');
git('add', '-A');
git('commit', '-q', '-m', `TimeTaker app, built from ${rev}${dirty ? ' (with uncommitted changes)' : ''}`);
console.log(`pushing to ${REMOTE} (${BRANCH}) …`);
git('push', '--force', REMOTE, `main:${BRANCH}`);
console.log('done. GitHub Pages publishes it within a minute or two.');
