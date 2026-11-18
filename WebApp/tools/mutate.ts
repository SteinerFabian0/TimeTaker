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
  // laps
  { name: 'min-lap check off', file: 'src/race/rules/laps.ts', from: 'sinceMs < config.minLapMs', to: 'false' },
  { name: 'min-lap default not 20 s', file: 'src/race/rules/config.ts', from: 'minLapMs: 20_000,', to: 'minLapMs: 2000,' },
  { name: 'IN_HOLDOFF ignored', file: 'src/race/rules/laps.ts', from: 'config.rejectInHoldoff && (c.flags & LaneEventFlag.IN_HOLDOFF)', to: 'false' },
  { name: 'ignoreCrossing ignored', file: 'src/race/raceEngine.ts', from: 'if (acts.ignored.has(posKey(c))) {', to: 'if (false) {' },
  { name: 'BLOCKED not a crossing', file: 'src/race/rules/timeline.ts', from: '(type !== LaneEventType.CROSSING && type !== LaneEventType.BLOCKED)', to: 'type !== LaneEventType.CROSSING' },
  { name: 'reboot: lap across boots timed', file: 'src/race/rules/laps.ts', from: 'const sameBoot = prev.bootId === c.bootId;', to: 'const sameBoot = true;' },
  { name: 'laneSeq gaps not flagged', file: 'src/race/rules/timeline.ts', from: 'st.lost = true;', to: 'st.lost = false;' },
  { name: 'crossings in log order, not tEntryUs', file: 'src/race/rules/timeline.ts', from: 'a.ord - b.ord || a.tUs - b.tUs || a.evSeq - b.evSeq', to: 'a.ord - b.ord || a.evSeq - b.evSeq' },
  // start
  { name: 'lap 1 timed from lights out, not the first crossing', file: 'src/race/raceEngine.ts', from: "car.prev = { bootId: c.bootId, tUs: c.tUs };\n          if (j.kind === 'falseStart') {", to: "car.prev = { bootId: start.bootId, tUs: tout };\n          if (j.kind === 'falseStart') {" },
  { name: 'aborted sequence still starts the race', file: 'src/race/rules/timeline.ts', from: '      pending = null;\n      start = null;\n    } else if (e.msgId === MsgId.EVT_BOOT', to: '    } else if (e.msgId === MsgId.EVT_BOOT' },
  { name: 'crossings before T0 not ignored', file: 'src/race/rules/timeline.ts', from: 'if (ord === startOrd && tUs < start.t0Us)', to: 'if (false)' },
  { name: 'pre-T0 crossing logged after SEQ_STARTED counted (old rule)', file: 'src/race/rules/timeline.ts', from: 'if (ord === startOrd && tUs < start.t0Us)', to: 'if (ord === startOrd && e.evSeq < start.seqEvSeq && tUs < start.t0Us)' },
  // false start
  { name: 'false start not judged (< Tout)', file: 'src/race/rules/start.ts', from: 'if (tUs < toutUs) {', to: 'if (false) {' },
  { name: 'false-start action default not timePenalty', file: 'src/race/rules/config.ts', from: "falseStart: { action: 'timePenalty'", to: "falseStart: { action: 'flag'" },
  { name: 'false-start penalty not added', file: 'src/race/rules/start.ts', from: "config.falseStart.action === 'timePenalty' ? config.falseStart.penaltyMs : 0", to: '0' },
  { name: 'false-start penalty not on the first lap', file: 'src/race/raceEngine.ts', from: 'if (car.laps.length === 0 && fsPenalty > 0)', to: 'if (false)' },
  { name: 'addToTotal also changes the first lap', file: 'src/race/rules/start.ts', from: "config.falseStart.apply === 'addToFirstLap' ? falseStartPenaltyMs(config) : 0", to: 'falseStartPenaltyMs(config)' },
  { name: 'stopAndGo accepted', file: 'src/race/rules/config.ts', from: "if (fs.apply === 'stopAndGo') p.push(", to: 'if (false) p.push(' },
  { name: 'false start → disqualify not applied', file: 'src/race/raceEngine.ts', from: "if (config.falseStart.action === 'disqualify' && !after(c, car.dqAt))", to: 'if (false)' },
  // finish
  { name: 'laps race: others do not finish on their next crossing', file: 'src/race/rules/finish.ts', from: '  if (flagOut) return true;\n', to: '' },
  { name: 'time race: flag never out', file: 'src/race/rules/finish.ts', from: 'return raceMs >= config.durationMs;', to: 'return false;' },
  { name: 'race ends without a car still on the grid', file: 'src/race/raceEngine.ts', from: 'const racing = cars.filter((c) => !out(c.status));', to: "const racing = cars.filter((c) => !out(c.status) && c.status !== 'grid');" },
  { name: 'race waits for disqualified / crashed cars', file: 'src/race/raceEngine.ts', from: "status === 'disqualified' || status === 'retired' || status === 'crashed';", to: "status === 'retired';" },
  // crashes
  { name: 'crashed/out ignored', file: 'src/race/raceEngine.ts', from: "if (after(c, acts.crashed.get(c.lane))) { reject('out'); continue; }", to: '' },
  { name: 'crashed car not out', file: 'src/race/raceEngine.ts', from: "acts.crashed.has(e.lane) ? 'crashed'", to: "false ? 'crashed'" },
  { name: 'crashed/out accepted under replacementAllowed', file: 'src/race/rules/actions.ts', from: "if (a.type === 'crashOut' && config.crashPolicy !== 'crashIsDnf') {", to: 'if (false) {' },
  // disqualification
  { name: 'laps after a DQ still count', file: 'src/race/raceEngine.ts', from: "if (after(c, car.dqAt)) { reject('disqualified'); continue; }", to: '' },
  { name: 'DQ from the start, not from its position', file: 'src/race/raceEngine.ts', from: 'at !== undefined && (at === null || cmp(c, at) > 0)', to: 'at !== undefined' },
  { name: 'lap in progress at a DQ not TLF', file: 'src/race/raceEngine.ts', from: 'tlfLap: dq && w.started && !w.finished ? w.laps.length + 1 : null', to: 'tlfLap: null' },
  { name: 'disqualified not ranked last', file: 'src/race/rules/standings.ts', from: 'if (classified(a) !== classified(b)) return classified(a) ? -1 : 1;', to: '' },
  // corrections and ranking
  { name: 'undo ignored', file: 'src/race/rules/actions.ts', from: '!(undoneBy.get(a.id) ?? []).some((u) => active.get(u))', to: 'true' },
  { name: 'lap adjustments ignored', file: 'src/race/rules/actions.ts', from: "case 'adjustLaps': r.lapAdjust.set(a.lane, (r.lapAdjust.get(a.lane) ?? 0) + a.delta); break;", to: "case 'adjustLaps': break;" },
  { name: 'penalties not in the total time', file: 'src/race/raceEngine.ts', from: 'totalMs: elapsedMs === null ? null : elapsedMs + penalties', to: 'totalMs: elapsedMs' },
  { name: 'tie: best lap not compared', file: 'src/race/rules/standings.ts', from: 'if (ba !== bb) return ba - bb;', to: '' },
  { name: 'tie: higher lane first', file: 'src/race/rules/standings.ts', from: 'return a.lane - b.lane;', to: 'return b.lane - a.lane;' },
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
