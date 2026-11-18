#!/usr/bin/env node
/*
 * Records the race fixtures: runs every scenario of tools/lib/raceScenarios.ts
 * against the simulator (virtual time, fixed seeds) and writes
 * test/fixtures/races/<name>.json in the race file format (src/store/raceFile.ts).
 *
 *   node tools/genFixtures.ts            all scenarios
 *   node tools/genFixtures.ts time-race  one
 *
 * Re-run only on purpose (a simulator or protocol change). The fixture test
 * checks that the stored files still replay to their `expect`, and that the
 * simulator still produces the same events byte for byte.
 */

import { mkdirSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { stringifyRace } from '../src/store/raceFile.ts';
import { runScenario, SCENARIOS } from './lib/raceScenarios.ts';

const dir = fileURLToPath(new URL('../test/fixtures/races/', import.meta.url));
mkdirSync(dir, { recursive: true });
const only = process.argv.slice(2);
for (const sc of SCENARIOS) {
  if (only.length && !only.includes(sc.name)) continue;
  const t0 = Date.now();
  const file = await runScenario(sc);
  writeFileSync(`${dir}${sc.name}.json`, stringifyRace(file));
  console.log(`${sc.name}: ${file.events.length} events, ${file.actions.length} actions (${Date.now() - t0} ms)`);
}
