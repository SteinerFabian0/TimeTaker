/*
 * tools/build.mjs: the import scanning and the .ts → .js rewrite the
 * production build relies on.
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

// @ts-ignore: a plain .mjs tool without types
import { specifiers, rewriteImports } from '../tools/build.mjs';

const src = `import { render } from 'preact';
import { html } from './lib/html.ts';
import './lib/prefs.ts';
export { x } from "../../src/a/b.ts";
import type { T } from './types.ts';
const m = await import('./lazy.ts');
const s = 'not an import: from a string';`;

describe('build', () => {
  test('finds static, side-effect, re-export and dynamic imports', () => {
    assert.deepEqual((specifiers(src) as string[]).sort(),
      ['../../src/a/b.ts', './lazy.ts', './lib/html.ts', './lib/prefs.ts', './types.ts', 'preact'].sort());
  });

  test('rewrites relative .ts imports to .js, leaves bare ones', () => {
    const out = rewriteImports(src) as string;
    assert.match(out, /from 'preact'/);
    assert.match(out, /from '\.\/lib\/html\.js'/);
    assert.match(out, /import '\.\/lib\/prefs\.js'/);
    assert.match(out, /from "\.\.\/\.\.\/src\/a\/b\.js"/);
    assert.match(out, /import\('\.\/lazy\.js'\)/);
    assert.doesNotMatch(out, /\.ts['"]/);
  });
});
