/*
 * The race engine's rules (RACE_RULES.md), one rule per case, on hand-built
 * device logs. Laps are about 30 s (the minimum lap is 20 s).
 * (Recorded races from the simulator are in raceFixtures.test.ts.)
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { fold } from '../src/race/raceEngine.ts';
import { DEFAULT_RACE, resolveConfig } from '../src/race/rules/config.ts';
import type { RaceConfigInput } from '../src/race/rules/config.ts';
import type { HostAction, RaceInputEvent } from '../src/race/types.ts';
import { LaneEventFlag, LaneEventType, LaneState, MsgId, SeqAbortReason } from '../src/protocol/generated.ts';

/** A device log builder. Times in ms (converted to µs). */
class Log {
  events: RaceInputEvent[] = [];
  bootId = 0xb001;
  #seq = 0;
  #laneSeq = new Map<number, number>();

  #push(msgId: number, value: Record<string, unknown>): this {
    this.events.push({ bootId: this.bootId, evSeq: this.#seq++, msgId, value: { evSeq: this.#seq - 1, ...value } });
    return this;
  }
  boot(bootId: number): this {
    this.bootId = bootId;
    this.#seq = 0;
    this.#laneSeq.clear();
    return this.#push(MsgId.EVT_BOOT, { resetCause: 1, bootId, sysUs: 0n });
  }
  seq(t0Ms: number, lights = 6): this { return this.#push(MsgId.EVT_SEQ_STARTED, { lights, armSampling: 1, stepMs: 1000, t0Us: BigInt(t0Ms * 1000) }); }
  out(toutMs: number): this { return this.#push(MsgId.EVT_LIGHTS_OUT, { lights: 6, toutUs: BigInt(toutMs * 1000) }); }
  abort(atMs: number): this { return this.#push(MsgId.EVT_SEQ_ABORTED, { reason: SeqAbortReason.HOST, atUs: BigInt(atMs * 1000) }); }
  lane(lane: number, state: number, atMs = 0): this { return this.#push(MsgId.EVT_LANE_STATE, { lane, state, status: 0, sysUs: BigInt(atMs * 1000) }); }
  x(lane: number, tMs: number, o: { flags?: number; type?: number; skipSeq?: number } = {}): this {
    const s = (this.#laneSeq.get(lane) ?? 0) + (o.skipSeq ?? 0);
    this.#laneSeq.set(lane, s + 1);
    return this.#push(MsgId.EVT_CROSSING, {
      lane, type: o.type ?? LaneEventType.CROSSING, flags: o.flags ?? 0, tEntryUs: BigInt(Math.round(tMs * 1000)),
      laneSeq: s & 0xffff, dwell10us: 2000, depth: 800,
    });
  }
  /** Laps of a car: crossings at start + k*lap. */
  laps(lane: number, firstMs: number, lapMs: number, n: number): this {
    for (let k = 0; k < n; k++) this.x(lane, firstMs + k * lapMs);
    return this;
  }
}

const cfg = (o: Partial<RaceConfigInput> = {}) => resolveConfig({
  id: 'r', entries: [{ lane: 1, driver: 'A' }, { lane: 2, driver: 'B' }], laps: 3, ...o,
});
const start = (): Log => new Log().boot(0xb001).seq(1000).out(8000);   // T0 1 s, Tout 8 s
let n = 0;
const act = (a: Record<string, unknown>): HostAction => ({ id: `a${++n}`, seq: n, at: null, ...a }) as HostAction;
const car = (s: ReturnType<typeof fold>, lane: number) => s.cars.find((c) => c.lane === lane)!;
const pos = (e: RaceInputEvent) => ({ bootId: e.bootId, evSeq: e.evSeq });
const lapMs = (s: ReturnType<typeof fold>, lane: number) => car(s, lane).lapTimes.map((l) => l.ms);

describe('settings (RACE_RULES.md defaults)', () => {
  test('every setting has its default', () => {
    const c = cfg();
    assert.deepEqual(c.falseStart, { action: 'timePenalty', penaltyMs: 5000, apply: 'addToFirstLap' });
    assert.equal(c.minLapMs, 20_000);
    assert.equal(c.crashPolicy, 'replacementAllowed');
    assert.deepEqual(c.startLights, { attentionPulses: 2, pulseRiseMs: 1000, stepMs: 1000, holdMinMs: 200, holdMaxMs: 3000 });
    assert.equal('firstCrossing' in DEFAULT_RACE, false);              // grid behind the line: a rule, not a setting
    assert.equal('blockedCountsAsCrossing' in DEFAULT_RACE, false);    // a stop on the line is a crossing: a rule
  });

  test('stopAndGo is a documented future value, rejected for now', () => {
    assert.throws(() => cfg({ falseStart: { apply: 'stopAndGo' } }), /stopAndGo needs the power board/);
  });

  test('nonsense is rejected', () => {
    assert.throws(() => cfg({ falseStart: { action: 'penalty' as never } }), /falseStart.action/);
    assert.throws(() => cfg({ crashPolicy: 'x' as never }), /crashPolicy/);
    assert.throws(() => cfg({ startLights: { holdMinMs: 3000, holdMaxMs: 200 } }), /hold range/);
    assert.throws(() => cfg({ startLights: { attentionPulses: 256 } }), /attentionPulses/);
    assert.throws(() => cfg({ startLights: { stepMs: 0 } }), /stepMs/);
    assert.throws(() => resolveConfig({ id: 'x', entries: [] }), /no entries/);
    assert.throws(() => resolveConfig({ id: 'x', entries: [{ lane: 1, driver: 'a' }, { lane: 1, driver: 'b' }] }), /twice/);
    assert.throws(() => resolveConfig({ id: 'x', entries: [{ lane: 7, driver: 'a' }] }), /1..6/);
    assert.throws(() => resolveConfig({ id: 'x', entries: [{ lane: 1, driver: 'a' }], mode: 'time', durationMs: 0 }), /durationMs/);
  });
});

describe('start', () => {
  test('waiting, then starting while the lights are on, running after lights out', () => {
    assert.equal(fold(cfg(), new Log().boot(1).events).phase, 'waiting');
    assert.equal(fold(cfg(), new Log().boot(1).seq(1000).events).phase, 'starting');
    const s = fold(cfg(), start().events);
    assert.equal(s.phase, 'running');
    assert.equal(s.start?.toutUs, 8_000_000);
  });

  test('grid behind the line: the first crossing starts lap 1 timing, it is not a lap', () => {
    const s = fold(cfg(), start().laps(1, 8600, 30_000, 3).events);
    const c = car(s, 1);
    assert.equal(c.reactionMs, 600);
    assert.equal(c.laps, 2);
    assert.deepEqual(lapMs(s, 1), [30_000, 30_000]);
    assert.equal(c.elapsedMs, 60_600);
  });

  test('crossings before the start sequence began are ignored, even when logged after it', () => {
    // a car positioned on the grid at 0.3 s; one more at 0.9 s reaches the log only after SEQ_STARTED
    const log = new Log().boot(1).x(1, 300).seq(1000).x(1, 900).out(8000).x(1, 8500).x(1, 38_500);
    const s = fold(cfg(), log.events);
    assert.equal(car(s, 1).falseStart, false);
    assert.equal(car(s, 1).reactionMs, 500);
    assert.deepEqual(lapMs(s, 1), [30_000]);
  });

  test('a crossing after T0 and before lights out is a false start: negative reaction, starts timing', () => {
    const s = fold(cfg({ falseStart: { action: 'flag' } }), start().x(1, 7500).x(1, 37_500).events);
    const c = car(s, 1);
    assert.equal(c.falseStart, true);
    assert.equal(c.reactionMs, -500);
    assert.deepEqual(lapMs(s, 1), [30_000]);
    assert.equal(c.penaltiesMs, 0);                                     // flag only
  });

  test('default: a 5 s time penalty added to the first lap time (and so to the total)', () => {
    const s = fold(cfg(), start().x(1, 7500).x(1, 37_500).x(1, 67_500).events);
    const c = car(s, 1);
    assert.deepEqual(c.lapTimes.map((l) => [l.ms, l.flags]), [[35_000, ['falseStartPenalty']], [30_000, []]]);
    assert.equal(c.penaltiesMs, 5000);
    assert.equal(c.totalMs, 59_500 + 5000);
    assert.equal(c.bestLapMs, 30_000);
  });

  test('penalty seconds setting and addToTotal: the first lap keeps its own time', () => {
    const s = fold(cfg({ falseStart: { penaltyMs: 3000, apply: 'addToTotal' } }), start().x(1, 7500).x(1, 37_500).events);
    assert.deepEqual(car(s, 1).lapTimes.map((l) => [l.ms, l.flags]), [[30_000, []]]);
    assert.equal(car(s, 1).penaltiesMs, 3000);
    assert.equal(car(s, 1).totalMs, 29_500 + 3000);
  });

  test('the time penalty changes the order', () => {
    const log = start().x(1, 7500).x(1, 37_500).x(2, 8700).x(2, 38_800).events;
    assert.equal(fold(cfg({ falseStart: { action: 'flag' } }), log).standings[0]?.lane, 1);
    assert.equal(fold(cfg(), log).standings[0]?.lane, 2);
  });

  test('false start → disqualify: from the false-start crossing; nothing after counts, lap 1 is TLF', () => {
    const log = start().x(1, 7500).x(1, 37_500).x(1, 67_500).x(2, 8700).x(2, 38_800).events;
    const s = fold(cfg({ falseStart: { action: 'disqualify' } }), log);
    const c = car(s, 1);
    assert.equal(c.status, 'disqualified');
    assert.equal(c.laps, 0);
    assert.equal(c.tlfLap, 1);
    assert.equal(c.penaltiesMs, 0);
    assert.deepEqual(c.rejected.map((r) => r.reason), ['disqualified', 'disqualified']);
    assert.equal(s.standings.at(-1)?.lane, 1);
  });

  test('a second crossing before lights out is rejected (beforeStart)', () => {
    const s = fold(cfg({ falseStart: { action: 'flag' } }), start().x(1, 5000).x(1, 7000).x(1, 35_000).events);
    assert.deepEqual(car(s, 1).rejected.map((r) => r.reason), ['beforeStart']);
    assert.deepEqual(lapMs(s, 1), [30_000]);
  });

  test('an aborted sequence does not count; the next one starts the race', () => {
    const log = new Log().boot(1).seq(1000).x(1, 3000).abort(3500).seq(10000).out(16000).x(1, 16500).x(1, 46_500);
    const s = fold(cfg(), log.events);
    assert.equal(s.aborts.length, 1);
    assert.equal(s.start?.toutUs, 16_000_000);
    assert.equal(car(s, 1).falseStart, false);
    assert.equal(car(s, 1).laps, 1);
  });
});

describe('laps', () => {
  test('crossing order is tEntryUs, not log order', () => {
    // lane 2 logged first but crossed later: lane 1 takes the flag.
    const log = start().x(1, 8500).x(2, 8600)
      .x(1, 38_500).x(2, 38_600).x(2, 68_600).x(1, 68_500).x(2, 98_600).x(1, 98_500);
    const s = fold(cfg(), log.events);
    assert.equal(car(s, 1).status, 'finished');
    assert.equal(s.standings[0]?.lane, 1);
    assert.equal(car(s, 2).status, 'finished');
  });

  test('minimum lap 20 s: a faster lap is discarded and the lap runs from the last counted crossing', () => {
    const s = fold(cfg({ laps: 10 }), start().x(1, 8500).x(1, 38_500).x(1, 58_499).x(1, 78_500).events);
    const c = car(s, 1);
    assert.deepEqual(c.rejected.map((r) => [r.reason, r.sinceMs]), [['minLap', 19_999]]);
    assert.deepEqual(c.lapTimes.map((l) => l.ms), [30_000, 40_000]);
  });

  test('exactly the minimum counts; the setting moves the limit', () => {
    const log = start().x(1, 8500).x(1, 28_500).x(1, 53_500).events;
    assert.deepEqual(lapMs(fold(cfg({ laps: 10 }), log), 1), [20_000, 25_000]);
    assert.deepEqual(lapMs(fold(cfg({ laps: 10, minLapMs: 21_000 }), log), 1), [45_000]);
  });

  test('there is no manual "accept anyway": an acceptCrossing action is ignored with a reason', () => {
    const log = start().x(1, 8500).x(1, 38_500).x(1, 40_000).x(1, 68_500);
    const short = log.events.at(-2)!;
    const s = fold(cfg({ laps: 10 }), log.events, [act({ type: 'acceptCrossing', crossing: pos(short) })]);
    assert.deepEqual(lapMs(s, 1), [30_000, 30_000]);
    assert.equal(s.appliedActions.length, 0);
    assert.match(s.ignoredActions[0]!.reason, /acceptCrossing/);
  });

  test('IN_HOLDOFF is rejected, unless rejectInHoldoff is off', () => {
    const log = start().x(1, 8500).x(1, 38_500, { flags: LaneEventFlag.IN_HOLDOFF }).x(1, 68_500).events;
    assert.deepEqual(car(fold(cfg(), log), 1).rejected.map((r) => r.reason), ['holdoff']);
    assert.equal(car(fold(cfg({ rejectInHoldoff: false }), log), 1).laps, 2);
  });

  test('a car stopped on the line (BLOCKED) counts as the crossing at its entry time; CLEARED never does', () => {
    const log = start().x(1, 8500).x(1, 38_500, { type: LaneEventType.BLOCKED }).x(1, 41_000, { type: LaneEventType.CLEARED })
      .x(1, 71_000).events;
    const s = fold(cfg(), log);
    assert.deepEqual(car(s, 1).lapTimes.map((l) => [l.ms, l.flags]), [[30_000, ['blocked']], [32_500, []]]);
  });

  test('flags: suspect timing, lost lane events, lane outage', () => {
    const log = start().x(1, 8500).x(1, 38_500, { flags: LaneEventFlag.EXTRAPOLATED })
      .x(1, 68_500, { skipSeq: 2 }).lane(1, LaneState.LEFT, 69_000).lane(1, LaneState.JOINED, 69_100).x(1, 98_500).events;
    const s = fold(cfg({ laps: 10 }), log);
    assert.deepEqual(car(s, 1).lapTimes.map((l) => l.flags), [['suspectTiming'], ['eventsLost'], ['laneOutage']]);
  });

  test('lanes outside the race are ignored', () => {
    const s = fold(cfg(), start().laps(3, 8500, 30_000, 4).events);
    assert.equal(s.cars.length, 2);
    assert.equal(s.cars.every((c) => c.laps === 0), true);
  });
});

describe('finish', () => {
  test('laps race: the leader takes the flag, others finish on their next crossing', () => {
    const log = start().laps(1, 8500, 30_000, 4).laps(2, 8700, 45_000, 2);   // B: 1 lap when A finishes (98.5 s)
    let s = fold(cfg(), log.events);
    assert.equal(car(s, 1).status, 'finished');
    assert.equal(car(s, 2).status, 'running');
    assert.equal(s.phase, 'finishing');
    s = fold(cfg(), log.x(2, 98_700).x(1, 128_500).events);
    assert.equal(car(s, 2).status, 'finished');
    assert.equal(car(s, 2).laps, 2);                                   // a lap down, finished on its next crossing
    assert.deepEqual(car(s, 1).rejected.map((r) => r.reason), ['afterFinish']);
    assert.equal(s.phase, 'finished');
    assert.deepEqual(s.standings.map((x) => [x.lane, x.laps]), [[1, 3], [2, 2]]);
    assert.deepEqual(s.standings[1]?.gap, { laps: 1, ms: null });
  });

  test('on the same lap, the gap is the difference in total time', () => {
    const s = fold(cfg(), start().laps(1, 8500, 30_000, 4).laps(2, 8700, 31_000, 4).events);
    assert.deepEqual(s.standings.map((x) => [x.lane, x.laps]), [[1, 3], [2, 3]]);
    assert.equal(s.standings[1]?.gap?.ms, 93_700 - 90_500);
  });

  test('time race: after the time, each car finishes on its next crossing', () => {
    const c = cfg({ mode: 'time', durationMs: 80_000 });
    const log = start().laps(1, 8500, 30_000, 4).laps(2, 8700, 28_000, 4);
    const s = fold(c, log.events);
    // A: race ms 0.5 30.5 60.5 90.5 → finishes at 90.5 s with 3 laps
    // B: race ms 0.7 28.7 56.7 84.7 → finishes at 84.7 s with 3 laps, earlier
    assert.equal(car(s, 2).status, 'finished');
    assert.equal(car(s, 1).status, 'finished');
    assert.equal(s.standings[0]?.lane, 2);
    assert.equal(s.phase, 'finished');
  });

  test('a time race shows the flag once the device clock passes the time', () => {
    const c = cfg({ mode: 'time', durationMs: 60_000 });
    const s = fold(c, start().x(1, 8500).lane(3, LaneState.SYNC_OK, 70_000).events);
    assert.equal(s.flagOut, true);
    assert.equal(s.phase, 'finishing');
  });

  test('the race ends only when every car still in the race has finished (a car on the grid too)', () => {
    const log = start().laps(1, 8500, 30_000, 4).events;               // lane 2 never left the grid
    assert.equal(fold(cfg(), log).phase, 'finishing');
    assert.equal(fold(cfg(), log, [act({ type: 'retire', lane: 2 })]).phase, 'finished');
  });
});

describe('crashes', () => {
  const race = (): Log => start().laps(1, 8500, 30_000, 4).laps(2, 8700, 31_000, 2);   // lane 2 crashes in lap 2

  test('replacement allowed: a long lap just counts; a crashed/out action is ignored with a reason', () => {
    const log = start().x(1, 8500).x(1, 98_500).x(1, 128_500).events;
    assert.deepEqual(lapMs(fold(cfg(), log), 1), [90_000, 30_000]);
    const s = fold(cfg(), race().events, [act({ type: 'crashOut', lane: 2 })]);
    assert.equal(car(s, 2).status, 'running');
    assert.equal(s.phase, 'finishing');                                 // the race still waits for lane 2
    assert.match(s.ignoredActions[0]!.reason, /replacementAllowed/);
  });

  test('crash = DNF: crashed/out leaves the race; laps before count; the race ends without it', () => {
    const log = race();
    const at = pos(log.events.at(-1)!);
    log.x(2, 120_000);                                                  // after the crash-out: never counts
    const s = fold(cfg({ crashPolicy: 'crashIsDnf' }), log.events, [act({ type: 'crashOut', lane: 2, at })]);
    const c = car(s, 2);
    assert.equal(c.status, 'crashed');
    assert.equal(c.laps, 1);
    assert.equal(c.tlfLap, null);
    assert.deepEqual(c.rejected.map((r) => r.reason), ['out']);
    assert.equal(s.phase, 'finished');
    assert.deepEqual(s.standings.map((x) => x.lane), [1, 2]);          // classified, by laps
  });
});

describe('disqualification', () => {
  const race = (): Log => start().laps(1, 8500, 30_000, 4).laps(2, 8700, 31_000, 2);

  test('from its log position: laps before count, later ones never; the lap in progress is TLF', () => {
    const log = race();
    const at = pos(log.events.at(-1)!);                                 // lane 2 is 1 lap in
    log.x(2, 70_700).x(2, 101_700);
    const s = fold(cfg(), log.events, [act({ type: 'disqualify', lane: 2, at })]);
    const c = car(s, 2);
    assert.equal(c.status, 'disqualified');
    assert.deepEqual(c.lapTimes.map((l) => l.ms), [31_000]);
    assert.equal(c.tlfLap, 2);
    assert.deepEqual(c.rejected.map((r) => r.reason), ['disqualified', 'disqualified']);
    assert.equal(s.phase, 'finished');                                  // the race no longer waits for it
  });

  test('ranks last; undo restores; undo of the undo applies again', () => {
    const dq = act({ type: 'disqualify', lane: 1 });
    const undo = act({ type: 'undo', target: dq.id });
    const redo = act({ type: 'undo', target: undo.id });
    assert.equal(fold(cfg({ laps: 10 }), race().events, [dq]).standings.at(-1)?.lane, 1);
    assert.equal(fold(cfg({ laps: 10 }), race().events, [dq, undo]).standings[0]?.lane, 1);
    const s = fold(cfg({ laps: 10 }), race().events, [dq, undo, redo]);
    assert.equal(car(s, 1).status, 'disqualified');
    assert.deepEqual(s.appliedActions, [dq.id, redo.id]);
  });

  test('no position (at null) = from the start: nothing counts', () => {
    const s = fold(cfg({ laps: 10 }), race().events, [act({ type: 'disqualify', lane: 1 })]);
    assert.equal(car(s, 1).laps, 0);
    assert.equal(car(s, 1).tlfLap, null);                              // it never started a lap that counted
  });

  test('a car disqualified after its finish keeps its result, no TLF', () => {
    const log = race();
    const s = fold(cfg(), log.events, [act({ type: 'disqualify', lane: 1, at: pos(log.events.at(-1)!) })]);
    assert.equal(car(s, 1).laps, 3);
    assert.equal(car(s, 1).tlfLap, null);
    assert.equal(car(s, 1).status, 'disqualified');
    assert.deepEqual(s.standings.map((x) => [x.lane, x.laps]), [[2, 1], [1, 3]]);   // DQ ranks last, whatever its laps
  });
});

describe('ranking', () => {
  test('penalty seconds change the order on equal laps', () => {
    const log = start().laps(1, 8500, 30_000, 3).laps(2, 8700, 30_200, 3);
    const s = fold(cfg({ laps: 10 }), log.events, [act({ type: 'penalty', lane: 1, ms: 1000 })]);
    assert.equal(car(s, 1).totalMs, 60_500 + 1000);
    assert.deepEqual(s.standings.map((x) => x.lane), [2, 1]);
  });

  test('exact tie: the faster best lap wins, even from the higher lane', () => {
    // both at race time 60 s; lane 2's best lap 29 s beats lane 1's 30 s
    const log = start().x(1, 8000).x(1, 38_000).x(1, 68_000).x(2, 8000).x(2, 39_000).x(2, 68_000);
    const s = fold(cfg({ laps: 10 }), log.events);
    assert.equal(car(s, 1).totalMs, car(s, 2).totalMs);
    assert.deepEqual(s.standings.map((x) => x.lane), [2, 1]);
  });

  test('equal totals tie exactly (µs arithmetic, no float noise); equal best lap too → lower lane', () => {
    // A: last crossing at race 60084.010 ms + 300 ms penalty; B: 60384.010 ms; same best lap
    const log = start().x(1, 8500).x(1, 38_500).x(1, 68_084.01).x(2, 8800).x(2, 38_800).x(2, 68_384.01);
    const s = fold(cfg({ laps: 10 }), log.events, [act({ type: 'penalty', lane: 1, ms: 300 })]);
    assert.equal(car(s, 1).totalMs, 60_384.01);
    assert.equal(car(s, 2).totalMs, 60_384.01);
    assert.equal(car(s, 1).bestLapMs, car(s, 2).bestLapMs);
    assert.deepEqual(s.standings.map((x) => x.lane), [1, 2]);
    assert.deepEqual(s.standings[1]?.gap, { laps: 0, ms: 0 });
  });

  test('a car still on the grid ranks after a started car with no lap yet', () => {
    const s = fold(cfg(), start().x(2, 8600).events);
    assert.deepEqual(s.standings.map((x) => x.lane), [2, 1]);
  });
});

describe('corrections', () => {
  const race = (): Log => start().laps(1, 8500, 30_000, 3).laps(2, 8700, 31_000, 3);

  test('+1 lap counts from the start and can bring the flag forward', () => {
    const s = fold(cfg(), race().events, [act({ type: 'adjustLaps', lane: 2, delta: 1 })]);
    assert.equal(car(s, 2).laps, 3);
    assert.equal(car(s, 2).status, 'finished');
    assert.equal(s.standings[0]?.lane, 2);
  });

  test('ignoreCrossing removes one crossing', () => {
    const log = start().x(1, 8500).x(1, 38_500).x(1, 68_500);
    const second = log.events.at(-2)!;
    const s = fold(cfg({ laps: 10 }), log.events, [act({ type: 'ignoreCrossing', crossing: pos(second) })]);
    assert.deepEqual(lapMs(s, 1), [60_000]);
    assert.deepEqual(car(s, 1).rejected.map((r) => r.reason), ['ignored']);
  });

  test('endRace freezes at its position', () => {
    const log = race();
    const cut = log.events[4]!;                                       // lane 1's 2nd crossing
    const s = fold(cfg({ laps: 10 }), log.events, [act({ type: 'endRace', at: pos(cut) })]);
    assert.equal(s.phase, 'finished');
    assert.equal(car(s, 1).laps, 1);
  });

  test('invalid actions are ignored with a reason', () => {
    const s = fold(cfg(), race().events, [
      act({ type: 'penalty', lane: 5, ms: 1000 }),
      act({ type: 'undo', target: 'nope' }),
      { ...act({ type: 'retire', lane: 1 }), id: 'dup' }, { ...act({ type: 'retire', lane: 2 }), id: 'dup' },
    ]);
    assert.equal(s.ignoredActions.length, 3);
    assert.equal(car(s, 1).status, 'retired');
    assert.equal(car(s, 2).status, 'running');                       // 2 laps; the retired car never takes the flag
  });
});

describe('device reboot mid-race', () => {
  test('a new bootId is a flagged timing gap; laps go on, the spanning lap has no time', () => {
    const log = start().laps(1, 8500, 30_000, 2);                     // 8.5, 38.5 (lap 1)
    log.boot(0xb002).x(1, 4000).x(1, 34_000);                         // new clock
    const s = fold(cfg({ laps: 10 }), log.events);
    const c = car(s, 1);
    assert.deepEqual(c.lapTimes.map((l) => [l.ms, l.flags]), [[30_000, []], [null, ['rebootGap']], [30_000, []]]);
    assert.equal(s.timingGaps.length, 1);
    assert.equal(s.timingGaps[0]?.atRaceMs, 30_500);
    assert.equal(c.elapsedMs, 30_500 + 34_000);
  });
});

describe('purity', () => {
  test('same inputs, same state; inputs untouched', () => {
    const log = start().laps(1, 8500, 30_000, 3).events;
    const actions = [act({ type: 'penalty', lane: 1, ms: 300 })];
    const copy = JSON.stringify(log, (_k, v: unknown) => (typeof v === 'bigint' ? v.toString() : v));
    assert.deepEqual(fold(cfg(), log, actions), fold(cfg(), log, actions));
    assert.equal(JSON.stringify(log, (_k, v: unknown) => (typeof v === 'bigint' ? v.toString() : v)), copy);
  });
});

describe('robustness (review 2026-11-18)', () => {
  const big = (_k: string, v: unknown): unknown => (typeof v === 'bigint' ? v.toString() : v);
  const race = (): Log => new Log().boot(0xb001).x(1, 300).seq(1000).x(2, 3000).abort(3500).seq(10000).out(16000)
    .x(1, 16500).x(2, 16700).lane(2, LaneState.LEFT, 18000).lane(2, LaneState.JOINED, 18500)
    .x(1, 46_500).x(1, 47_000).x(2, 46_800).x(1, 76_500, { skipSeq: 1 }).x(2, 76_900);

  test('a device event delivered twice changes nothing (idempotent on bootId+evSeq)', () => {
    const clean = race().events;
    const twice = clean.flatMap((e, i) => (i % 2 ? [e, { ...e }] : [e, e]));
    assert.equal(JSON.stringify(fold(cfg(), twice), big), JSON.stringify(fold(cfg(), clean), big));
  });

  test('events of a boot delivered out of evSeq order fold like the log order', () => {
    const clean = race().events;
    const shuffled = [clean[0]!, ...clean.slice(1).reverse()];
    assert.equal(JSON.stringify(fold(cfg(), shuffled), big), JSON.stringify(fold(cfg(), clean), big));
  });

  test('ignoreCrossing works on the start crossing and on a false-start crossing', () => {
    // a ghost first crossing would otherwise be the start crossing (and make the real one a min-lap reject)
    const log = start().x(1, 8300).x(1, 8600).x(1, 38_600);
    const ghost = log.events.at(-3)!;
    const s = fold(cfg(), log.events, [act({ type: 'ignoreCrossing', crossing: pos(ghost) })]);
    assert.equal(car(s, 1).reactionMs, 600);
    assert.deepEqual(lapMs(s, 1), [30_000]);
    assert.deepEqual(car(s, 1).rejected.map((r) => r.reason), ['ignored']);
    // a hand in the beam before lights out, ignored by the marshal: no false start, no penalty
    const fs = start().x(1, 7900).x(1, 8600).x(1, 38_600);
    const hand = fs.events.at(-3)!;
    const f = fold(cfg(), fs.events, [act({ type: 'ignoreCrossing', crossing: pos(hand) })]);
    assert.equal(car(f, 1).falseStart, false);
    assert.equal(car(f, 1).penaltiesMs, 0);
    assert.equal(car(f, 1).reactionMs, 600);
    assert.deepEqual(lapMs(f, 1), [30_000]);
  });

  test('an ignoreCrossing naming no crossing is ignored with a reason, not applied', () => {
    const log = start().laps(1, 8500, 30_000, 3);
    const lightsOut = log.events[2]!;
    const missing = act({ type: 'ignoreCrossing', crossing: { bootId: 0xb001, evSeq: 999 } });
    const notCrossing = act({ type: 'ignoreCrossing', crossing: pos(lightsOut) });
    const s = fold(cfg({ laps: 10 }), log.events, [missing, notCrossing]);
    assert.deepEqual(s.appliedActions, []);
    assert.deepEqual(s.ignoredActions.map((x) => x.id), [missing.id, notCrossing.id]);
    assert.equal(car(s, 1).laps, 2);
  });

  test('an ignored crossing still hands its lane outage to the next lap', () => {
    const log = start().x(1, 8500).lane(1, LaneState.LEFT, 9000).lane(1, LaneState.JOINED, 9100).x(1, 38_500).x(1, 68_500);
    const mid = log.events.at(-2)!;
    const s = fold(cfg(), log.events, [act({ type: 'ignoreCrossing', crossing: pos(mid) })]);
    assert.deepEqual(car(s, 1).lapTimes.map((l) => [l.ms, l.flags]), [[60_000, ['laneOutage']]]);
  });
});
