#!/usr/bin/env node
/*
 * Mutation check of the race rules and the health / diagnostics rules:
 * breaks one rule at a time in a temporary copy of WebApp/ and runs the
 * tests of that area (race: unit + recorded fixtures; health: rules,
 * monitor, simulator diagnostics). Every mutation must make at least one
 * test fail.
 *
 *   node tools/mutate.ts
 */

import { execFileSync } from 'node:child_process';
import { cpSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('..', import.meta.url));

const RACE_TESTS = ['test/raceEngine.test.ts', 'test/raceFixtures.test.ts'];
const HEALTH_TESTS = ['test/health.test.ts', 'test/monitor.test.ts', 'test/simDiagnostics.test.ts'];
const H = 'src/health/health.ts';
const M = 'src/health/monitor.ts';

const MUTATIONS: { name: string; file: string; from: string; to: string; tests?: string[] }[] = [
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
  // health rules (src/health/health.ts)
  { name: 'health: upper bounds exclusive (> instead of >=)', file: H, from: "return v >= fail ? 'fail' : v >= warn ? 'warn' : 'ok';", to: "return v > fail ? 'fail' : v > warn ? 'warn' : 'ok';", tests: HEALTH_TESTS },
  { name: 'health: lower bounds inclusive (<= instead of <)', file: H, from: "return v < fail ? 'fail' : v < warn ? 'warn' : 'ok';", to: "return v <= fail ? 'fail' : v <= warn ? 'warn' : 'ok';", tests: HEALTH_TESTS },
  { name: 'health: drift warn at 80 %', file: H, from: 'driftWarn: 0.85,', to: 'driftWarn: 0.80,', tests: HEALTH_TESTS },
  { name: 'health: drift fail at 60 %', file: H, from: 'driftFail: 0.70,', to: 'driftFail: 0.60,', tests: HEALTH_TESTS },
  { name: 'health: drift high side ignored', file: H, from: 'else if (r > t.driftHighWarn) {', to: 'else if (false) {', tests: HEALTH_TESTS },
  { name: 'health: noise ratio never judged', file: H, from: 'const lvl = upper(r, t.noiseWarn, t.noiseFail);', to: "const lvl = 'ok' as const;", tests: HEALTH_TESTS },
  { name: 'health: saturation ignored', file: H, from: 'else if (peak >= t.saturationFail)', to: 'else if (false)', tests: HEALTH_TESTS },
  { name: 'health: low signal ignored', file: H, from: 'const lvl = lower(ls.baseline, t.signalLowWarn, t.signalLowFail);', to: "const lvl = 'ok' as const;", tests: HEALTH_TESTS },
  { name: 'health: SENSOR_OK ignored', file: H, from: "out.push(has(LaneStatusFlag.SENSOR_OK) ?", to: 'out.push(true ?', tests: HEALTH_TESTS },
  { name: 'health: SYNC lost ignored', file: H, from: 'if (has(LaneStatusFlag.SYNC_LOST) || !node.syncOk)', to: 'if (false)', tests: HEALTH_TESTS },
  { name: 'health: lane fault ignored', file: H, from: "if (node.phase === 'fault') out.push(", to: 'if (false) out.push(', tests: HEALTH_TESTS },
  { name: 'health: failed calibration not a failure', file: H, from: "if (cal?.result === 'failed') return mk(id, 'fail',", to: "if (cal?.result === 'failed') return mk(id, 'ok',", tests: HEALTH_TESTS },
  { name: 'health: forgotten (no LANE_STATUS) judged ok', file: H, from: ": mk(id, 'warn', 'no saved reference: the lane learns", to: ": mk(id, 'ok', 'no saved reference: the lane learns", tests: HEALTH_TESTS },
  { name: 'health: expected lane absent not a failure', file: H, from: "mk('lane.present', expected ? 'fail' : 'unknown',", to: "mk('lane.present', 'unknown',", tests: HEALTH_TESTS },
  { name: 'health: BLE bridge absent ok', file: H, from: "if (st.bleBridge === BleBridgeState.ABSENT) out.push(mk('ble.bridge', 'fail'", to: "if (false) out.push(mk('ble.bridge', 'fail'", tests: HEALTH_TESTS },
  { name: 'health: BLE connected ignores the phone link', file: H, from: 'const connected = online && st.bleLink === BleLinkState.UP;', to: 'const connected = online;', tests: HEALTH_TESTS },
  { name: 'health: RSSI not judged', file: H, from: 'lower(st.bleRssi, t.rssiWarnDbm, t.rssiFailDbm)', to: "'ok'", tests: HEALTH_TESTS },
  { name: 'health: stale STATUS still online', file: H, from: 'inputs.statusAgeMs <= t.statusStaleMs;', to: 'true;', tests: HEALTH_TESTS },
  { name: 'health: overall: a failing check only warns', file: H, from: "const overall: OverallLevel = w === 'unknown' ? 'ok' : w;", to: "const overall: OverallLevel = w === 'unknown' ? 'ok' : w === 'fail' ? 'warn' : w;", tests: HEALTH_TESTS },
  { name: 'health: events overwritten unread not a failure', file: H, from: '} else if (app < st.logOldestSeq) {', to: '} else if (false) {', tests: HEALTH_TESTS },
  // monitor (src/health/monitor.ts)
  { name: 'monitor: no slow rate during a race', file: M, from: 'laneRateHz(): number { return this.busy() ? this.#busyRate : this.#rate; }', to: 'laneRateHz(): number { return this.#rate; }', tests: HEALTH_TESTS },
  { name: 'monitor: calibrate allowed during a race', file: M, from: 'if (this.busy()) throw new MonitorBusyError(what);', to: '', tests: HEALTH_TESTS },
  { name: 'monitor: device seqRunning not a gate', file: M, from: ' || this.#nodes.seqRunning || this.#status?.seqRunning === 1;', to: ';', tests: HEALTH_TESTS },
  { name: 'monitor: two LANE_STATUS of a lane in flight', file: M, from: '    if (cur) return cur;\n', to: '', tests: HEALTH_TESTS },
  { name: 'monitor: calibration never times out', file: M, from: '        }, timeoutMs);', to: '        }, timeoutMs * 1000);', tests: HEALTH_TESTS },
  { name: 'monitor: ring never pruned by age', file: M, from: 'while (this.#size > 0 && this.#buf[this.#head]!.tMs < tMs) {', to: 'while (false) {', tests: HEALTH_TESTS },
  { name: 'monitor: FORGET skips absent lanes', file: M, from: 'const send = action === CalibrateAction.FORGET ? wanted : wanted.filter', to: 'const send = wanted.filter', tests: HEALTH_TESTS },
  // node model and simulator (0.6)
  { name: 'nodeModel: CONFIGURED does not clear a fault', file: 'src/device/nodeModel.ts', from: "case LaneState.CONFIGURED: next = { ...cur, phase: 'configured', faultStatus: 0,", to: "case LaneState.CONFIGURED: next = { ...cur, phase: cur.phase === 'fault' ? 'fault' : 'configured',", tests: HEALTH_TESTS },
  { name: 'sim: rawMin/rawMax window not reset by a read', file: 'src/device/simDevice.ts', from: '    s.winFromUs = nowUs + 1;\n    let flags = 0;', to: '    let flags = 0;', tests: HEALTH_TESTS },
  { name: 'sim: LANE_CALIBRATE accepted during a sequence', file: 'src/device/simDevice.ts', from: "if (this.#seq) { reply(Status.ERR_SEQ_RUNNING); return; }\n        if (action", to: 'if (action', tests: HEALTH_TESTS },
  { name: 'health: savedReference 0 judged ok', file: 'src/health/health.ts', from: "if (saved === 0) return mk(id, 'warn'", to: "if (saved === 0) return mk(id, 'ok'", tests: HEALTH_TESTS },
  { name: 'health: another value than the saved one judged ok', file: 'src/health/health.ts', from: "return mk(id, 'warn', `lane runs on", to: "return mk(id, 'ok', `lane runs on", tests: HEALTH_TESTS },
  { name: 'health: a failed calibration no longer wins over LANE_STATUS', file: 'src/health/health.ts', from: "if (cal?.result === 'failed') return", to: "if (false) return", tests: HEALTH_TESTS },
  { name: 'sim: savedReference ignores the kept RAM value', file: 'src/device/simDevice.ts', from: 'return this.#keptRefs.get(lane) ?? this.#flash.references[lane] ?? 0;', to: 'return this.#flash.references[lane] ?? 0;', tests: HEALTH_TESTS },
  { name: 'sim: saved reference not seeded at join', file: 'src/device/simDevice.ts', from: '    if (saved) {\n      s.learning = null;', to: '    if (false) {\n      s.learning = null;', tests: HEALTH_TESTS },
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
      // A mutant can make a test wait forever (e.g. a timeout that never fires): that counts as a failing test.
      execFileSync(process.execPath, ['--test', '--test-timeout=60000', ...(m.tests ?? RACE_TESTS)], { cwd: tmp, stdio: 'pipe', timeout: 300_000 });
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
