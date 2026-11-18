#!/usr/bin/env node
/*
 * Mutation check of the race rules: breaks one rule at a time in a temporary
 * copy of WebApp/ and runs the race tests (unit + recorded fixtures). Every
 * mutation must make at least one test fail.
 *
 *   node tools/mutate.ts
 */

import { execFileSync } from 'node:child_process';
import { cpSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('..', import.meta.url));

const MUTATIONS: { name: string; file: string; from: string; to: string }[] = [
  { name: 'min-lap check off', file: 'src/race/rules/laps.ts', from: 'sinceMs < config.minLapMs', to: 'false' },
  { name: 'IN_HOLDOFF ignored', file: 'src/race/rules/laps.ts', from: 'config.rejectInHoldoff && (c.flags & LaneEventFlag.IN_HOLDOFF)', to: 'false' },
  { name: 'ignoreCrossing ignored', file: 'src/race/rules/laps.ts', from: "if (ctx.ignored) return { ok: false, reason: 'ignored', sinceMs };", to: '' },
  { name: 'false start not judged (< Tout)', file: 'src/race/rules/start.ts', from: 'if (tUs < toutUs) {', to: 'if (false) {' },
  { name: 'first crossing counts as a lap', file: 'src/race/rules/start.ts', from: "isStartCrossing: startsTiming };\n  return { kind: 'race' };", to: "isStartCrossing: false };\n  return { kind: 'race' };" },
  { name: 'false-start penalty not added', file: 'src/race/rules/start.ts', from: "config.falseStart.action === 'penalty' ? config.falseStart.penaltyMs : 0", to: '0' },
  { name: 'crossings in log order, not tEntryUs', file: 'src/race/rules/timeline.ts', from: 'a.ord - b.ord || a.tUs - b.tUs || a.evSeq - b.evSeq', to: 'a.ord - b.ord || a.evSeq - b.evSeq' },
  { name: 'aborted sequence still starts the race', file: 'src/race/rules/timeline.ts', from: '      pending = null;\n      start = null;\n    } else if (e.msgId === MsgId.EVT_BOOT', to: '    } else if (e.msgId === MsgId.EVT_BOOT' },
  { name: 'practice crossings not filtered', file: 'src/race/rules/timeline.ts', from: 'if (ord === startOrd && e.evSeq < start.seqEvSeq && tUs < start.t0Us)', to: 'if (false)' },
  { name: 'laps race: others do not finish on their next crossing', file: 'src/race/rules/finish.ts', from: '  if (flagOut) return true;\n', to: '' },
  { name: 'time race: flag never out', file: 'src/race/rules/finish.ts', from: 'return raceMs >= config.durationMs;', to: 'return false;' },
  { name: 'undo ignored', file: 'src/race/rules/actions.ts', from: '!(undoneBy.get(a.id) ?? []).some((u) => active.get(u))', to: 'true' },
  { name: 'lap adjustments ignored', file: 'src/race/rules/actions.ts', from: "case 'adjustLaps': r.lapAdjust.set(a.lane, (r.lapAdjust.get(a.lane) ?? 0) + a.delta); break;", to: "case 'adjustLaps': break;" },
  { name: 'penalties not in the total time', file: 'src/race/raceEngine.ts', from: 'totalMs: elapsedMs === null ? null : elapsedMs + penalties', to: 'totalMs: elapsedMs' },
  { name: 'disqualified not ranked last', file: 'src/race/rules/standings.ts', from: 'if (classified(a) !== classified(b)) return classified(a) ? -1 : 1;', to: '' },
  { name: 'reboot: lap across boots timed', file: 'src/race/rules/laps.ts', from: 'const sameBoot = prev.bootId === c.bootId;', to: 'const sameBoot = true;' },
  { name: 'laneSeq gaps not flagged', file: 'src/race/rules/timeline.ts', from: 'st.lost = true;', to: 'st.lost = false;' },
];

const tmp = mkdtempSync(join(tmpdir(), 'tt-mutate-'));
let survived = 0;
try {
  for (const d of ['src', 'test', 'tools', 'package.json']) cpSync(join(ROOT, d), join(tmp, d), { recursive: true });
  console.log('| Mutation | Failing tests |\n|---|---|');
  for (const m of MUTATIONS) {
    const path = join(tmp, m.file);
    const orig = readFileSync(path, 'utf8');
    if (!orig.includes(m.from)) { console.log(`| ${m.name} | MUTATION DOES NOT APPLY |`); survived++; continue; }
    writeFileSync(path, orig.replace(m.from, m.to));
    let failed = 0;
    try {
      execFileSync(process.execPath, ['--test', 'test/raceEngine.test.ts', 'test/raceFixtures.test.ts'], { cwd: tmp, stdio: 'pipe' });
    } catch (e) {
      const out = String((e as { stdout?: Buffer }).stdout ?? '');
      failed = Number(/ℹ fail (\d+)/.exec(out)?.[1] ?? 1);
    }
    if (failed === 0) survived++;
    console.log(`| ${m.name} | ${failed === 0 ? 'SURVIVED' : failed} |`);
    writeFileSync(path, orig);
  }
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
console.log(survived ? `\n${survived} mutation(s) survived` : '\nevery mutation was caught');
process.exit(survived ? 1 : 0);
