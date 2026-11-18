/*
 * The race engine's rules, one rule per case, on hand-built device logs.
 * (Recorded races from the simulator are in raceFixtures.test.ts.)
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { fold } from '../src/race/raceEngine.ts';
import { resolveConfig } from '../src/race/rules/config.ts';
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

describe('start', () => {
  test('waiting, then starting while the lights are on, running after lights out', () => {
    assert.equal(fold(cfg(), new Log().boot(1).events).phase, 'waiting');
    assert.equal(fold(cfg(), new Log().boot(1).seq(1000).events).phase, 'starting');
    const s = fold(cfg(), start().events);
    assert.equal(s.phase, 'running');
    assert.equal(s.start?.toutUs, 8_000_000);
  });

  test('startsTiming: the first crossing starts lap 1, reaction = crossing − Tout', () => {
    const s = fold(cfg(), start().laps(1, 8600, 5000, 3).events);
    const c = car(s, 1);
    assert.equal(c.reactionMs, 600);
    assert.equal(c.laps, 2);
    assert.deepEqual(c.lapTimes.map((l) => l.ms), [5000, 5000]);
    assert.equal(c.elapsedMs, 10600);
  });

  test('completesLap1: lap 1 runs from Tout', () => {
    const s = fold(cfg({ firstCrossing: 'completesLap1' }), start().laps(1, 12000, 5000, 2).events);
    assert.deepEqual(car(s, 1).lapTimes.map((l) => l.ms), [4000, 5000]);
    assert.equal(car(s, 1).reactionMs, 4000);
  });

  test('a crossing before lights out is a false start with a negative reaction, and starts timing', () => {
    const s = fold(cfg(), start().x(1, 7500).x(1, 12500).events);
    const c = car(s, 1);
    assert.equal(c.falseStart, true);
    assert.equal(c.reactionMs, -500);
    assert.deepEqual(c.lapTimes.map((l) => l.ms), [5000]);
    assert.equal(c.penaltiesMs, 0);                                     // default action: flag
  });

  test('false start actions: penalty adds time, disqualify classifies last', () => {
    const log = start().x(1, 7500).x(1, 12500).x(2, 8700).x(2, 13800).events;
    const pen = fold(cfg({ falseStart: { action: 'penalty', penaltyMs: 5000 } }), log);
    assert.equal(car(pen, 1).totalMs, 4500 + 5000);
    assert.equal(pen.standings[0]?.lane, 2);
    const dq = fold(cfg({ falseStart: { action: 'disqualify' } }), log);
    assert.equal(car(dq, 1).status, 'disqualified');
    assert.equal(dq.standings.at(-1)?.lane, 1);
  });

  test('a second crossing before lights out is rejected (beforeStart)', () => {
    const s = fold(cfg(), start().x(1, 5000).x(1, 7000).x(1, 11000).events);
    assert.deepEqual(car(s, 1).rejected.map((r) => r.reason), ['beforeStart']);
    assert.deepEqual(car(s, 1).lapTimes.map((l) => l.ms), [6000]);
  });

  test('practice crossings before the sequence are ignored', () => {
    const log = new Log().boot(1).x(1, 300).x(1, 400).seq(1000).out(8000).x(1, 8500).x(1, 13500);
    const s = fold(cfg(), log.events);
    assert.equal(car(s, 1).falseStart, false);
    assert.equal(car(s, 1).laps, 1);
  });

  test('an aborted sequence does not count; the next one starts the race', () => {
    const log = new Log().boot(1).seq(1000).x(1, 3000).abort(3500).seq(10000).out(16000).x(1, 16500).x(1, 21500);
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
      .x(1, 13500).x(2, 13600).x(2, 18600).x(1, 18500).x(2, 23600).x(1, 23500);
    const s = fold(cfg(), log.events);
    assert.equal(car(s, 1).status, 'finished');
    assert.equal(s.standings[0]?.lane, 1);
    assert.equal(car(s, 2).status, 'finished');
  });

  test('min-lap: a too-short crossing is rejected and the lap runs from the last counted one', () => {
    const s = fold(cfg(), start().x(1, 8500).x(1, 13500).x(1, 15000).x(1, 19500).events);
    const c = car(s, 1);
    assert.deepEqual(c.rejected.map((r) => [r.reason, r.sinceMs]), [['minLap', 1500]]);
    assert.deepEqual(c.lapTimes.map((l) => l.ms), [5000, 6000]);
  });

  test('IN_HOLDOFF is rejected, unless rejectInHoldoff is off', () => {
    const log = start().x(1, 8500).x(1, 13500, { flags: LaneEventFlag.IN_HOLDOFF }).x(1, 18500).events;
    assert.deepEqual(car(fold(cfg(), log), 1).rejected.map((r) => r.reason), ['holdoff']);
    assert.equal(car(fold(cfg({ rejectInHoldoff: false }), log), 1).laps, 2);
  });

  test('BLOCKED counts at its entry time by default; CLEARED never does', () => {
    const log = start().x(1, 8500).x(1, 13500, { type: LaneEventType.BLOCKED }).x(1, 16000, { type: LaneEventType.CLEARED })
      .x(1, 21000).events;
    const s = fold(cfg(), log);
    assert.deepEqual(car(s, 1).lapTimes.map((l) => [l.ms, l.flags]), [[5000, ['blocked']], [7500, []]]);
    assert.equal(car(fold(cfg({ blockedCountsAsCrossing: false }), log), 1).laps, 1);
  });

  test('flags: suspect timing, lost lane events, lane outage', () => {
    const log = start().x(1, 8500).x(1, 13500, { flags: LaneEventFlag.EXTRAPOLATED })
      .x(1, 18500, { skipSeq: 2 }).lane(1, LaneState.LEFT, 19000).lane(1, LaneState.JOINED, 19100).x(1, 28500).events;
    const s = fold(cfg({ laps: 10 }), log);
    assert.deepEqual(car(s, 1).lapTimes.map((l) => l.flags), [['suspectTiming'], ['eventsLost'], ['laneOutage']]);
  });

  test('lanes outside the race are ignored', () => {
    const s = fold(cfg(), start().laps(3, 8500, 5000, 4).events);
    assert.equal(s.cars.length, 2);
    assert.equal(s.cars.every((c) => c.laps === 0), true);
  });
});

describe('finish', () => {
  test('laps race: the leader takes the flag, others finish on their next crossing', () => {
    const log = start().laps(1, 8500, 5000, 4).laps(2, 8700, 6000, 3);   // B: 3 crossings → 2 laps, before A finishes
    let s = fold(cfg(), log.events);
    assert.equal(car(s, 1).status, 'finished');
    assert.equal(car(s, 2).status, 'running');
    assert.equal(s.phase, 'finishing');
    s = fold(cfg(), log.x(2, 26700).x(1, 28500).events);
    assert.equal(car(s, 2).status, 'finished');
    assert.equal(car(s, 2).laps, 3);
    assert.deepEqual(car(s, 1).rejected.map((r) => r.reason), ['afterFinish']);
    assert.equal(s.phase, 'finished');
    assert.deepEqual(s.standings.map((x) => [x.lane, x.laps]), [[1, 3], [2, 3]]);
    assert.equal(s.standings[1]?.gap?.ms, 18700 - 15500);
  });

  test('time race: after the time, each car finishes on its next crossing', () => {
    const c = cfg({ mode: 'time', durationMs: 12000 });
    const log = start().laps(1, 8500, 5000, 4).laps(2, 8700, 4000, 4);
    const s = fold(c, log.events);
    // A: 8.5 13.5 18.5 23.5 → race ms 15500 ≥ 12000 at the 4th crossing (lap 3)
    // B: 8.7 12.7 16.7 20.7 → 12700 ms at the 4th: finished with 3 laps, earlier
    assert.equal(car(s, 2).status, 'finished');
    assert.equal(car(s, 1).status, 'finished');
    assert.equal(s.standings[0]?.lane, 2);
    assert.equal(s.phase, 'finished');
  });

  test('a time race shows the flag once the device clock passes the time', () => {
    const c = cfg({ mode: 'time', durationMs: 12000 });
    const s = fold(c, start().x(1, 8500).lane(3, LaneState.SYNC_OK, 21000).events);
    assert.equal(s.flagOut, true);
    assert.equal(s.phase, 'finishing');
  });
});

describe('corrections', () => {
  const race = (): Log => start().laps(1, 8500, 5000, 3).laps(2, 8700, 5200, 3);

  test('+1 lap counts from the start and can bring the flag forward', () => {
    const s = fold(cfg(), race().events, [act({ type: 'adjustLaps', lane: 2, delta: 1 })]);
    assert.equal(car(s, 2).laps, 3);
    assert.equal(car(s, 2).status, 'finished');
    assert.equal(s.standings[0]?.lane, 2);
  });

  test('penalty seconds change the order on equal laps', () => {
    const s = fold(cfg({ laps: 10 }), race().events, [act({ type: 'penalty', lane: 1, ms: 1000 })]);
    assert.equal(car(s, 1).totalMs, 10500 + 1000);
    assert.deepEqual(s.standings.map((x) => x.lane), [2, 1]);
  });

  test('disqualify ranks last; undo restores; undo of the undo applies again', () => {
    const dq = act({ type: 'disqualify', lane: 1 });
    const undo = act({ type: 'undo', target: dq.id });
    const redo = act({ type: 'undo', target: undo.id });
    assert.equal(fold(cfg({ laps: 10 }), race().events, [dq]).standings.at(-1)?.lane, 1);
    assert.equal(fold(cfg({ laps: 10 }), race().events, [dq, undo]).standings[0]?.lane, 1);
    const s = fold(cfg({ laps: 10 }), race().events, [dq, undo, redo]);
    assert.equal(car(s, 1).status, 'disqualified');
    assert.deepEqual(s.appliedActions, [dq.id, redo.id]);
  });

  test('ignoreCrossing removes a lap; acceptCrossing forces a rejected one in', () => {
    const log = start().x(1, 8500).x(1, 13500).x(1, 15000).x(1, 20000);
    const third = log.events.at(-2)!;
    const s0 = fold(cfg({ laps: 10 }), log.events);
    assert.deepEqual(car(s0, 1).lapTimes.map((l) => l.ms), [5000, 6500]);
    const acc = fold(cfg({ laps: 10 }), log.events, [act({ type: 'acceptCrossing', crossing: { bootId: third.bootId, evSeq: third.evSeq } })]);
    assert.deepEqual(car(acc, 1).lapTimes.map((l) => [l.ms, l.flags]), [[5000, []], [1500, ['accepted']], [5000, []]]);
    const second = log.events.at(-3)!;
    const ign = fold(cfg({ laps: 10 }), log.events, [act({ type: 'ignoreCrossing', crossing: { bootId: second.bootId, evSeq: second.evSeq } })]);
    assert.deepEqual(car(ign, 1).lapTimes.map((l) => l.ms), [6500, 5000]);   // 15.0 is now 6.5 s after 8.5
  });

  test('endRace freezes at its position', () => {
    const log = race();
    const cut = log.events[4]!;                                       // lane 1's 2nd crossing
    const s = fold(cfg({ laps: 10 }), log.events, [act({ type: 'endRace', at: { bootId: cut.bootId, evSeq: cut.evSeq } })]);
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
    const log = start().laps(1, 8500, 5000, 2);                       // 8.5, 13.5 (lap 1)
    log.boot(0xb002).x(1, 4000).x(1, 9000);                           // new clock
    const s = fold(cfg({ laps: 10 }), log.events);
    const c = car(s, 1);
    assert.deepEqual(c.lapTimes.map((l) => [l.ms, l.flags]), [[5000, []], [null, ['rebootGap']], [5000, []]]);
    assert.equal(s.timingGaps.length, 1);
    assert.equal(s.timingGaps[0]?.atRaceMs, 5500);
    assert.equal(c.elapsedMs, 5500 + 9000);
  });
});

describe('purity', () => {
  test('same inputs, same state; inputs untouched', () => {
    const log = start().laps(1, 8500, 5000, 3).events;
    const actions = [act({ type: 'penalty', lane: 1, ms: 300 })];
    const copy = JSON.stringify(log, (_k, v: unknown) => (typeof v === 'bigint' ? v.toString() : v));
    assert.deepEqual(fold(cfg(), log, actions), fold(cfg(), log, actions));
    assert.equal(JSON.stringify(log, (_k, v: unknown) => (typeof v === 'bigint' ? v.toString() : v)), copy);
  });

  test('config validation', () => {
    assert.throws(() => resolveConfig({ id: 'x', entries: [] }), /no entries/);
    assert.throws(() => resolveConfig({ id: 'x', entries: [{ lane: 1, driver: 'a' }, { lane: 1, driver: 'b' }] }), /twice/);
    assert.throws(() => resolveConfig({ id: 'x', entries: [{ lane: 7, driver: 'a' }] }), /1..6/);
    assert.throws(() => resolveConfig({ id: 'x', entries: [{ lane: 1, driver: 'a' }], mode: 'time', durationMs: 0 }), /durationMs/);
  });
});

describe('robustness (review 2026-11-18)', () => {
  const big = (_k: string, v: unknown): unknown => (typeof v === 'bigint' ? v.toString() : v);
  const race = (): Log => new Log().boot(0xb001).x(1, 300).seq(1000).x(2, 3000).abort(3500).seq(10000).out(16000)
    .x(1, 16500).x(2, 16700).lane(2, LaneState.LEFT, 18000).lane(2, LaneState.JOINED, 18500)
    .x(1, 21500).x(1, 22000).x(2, 21800).x(1, 26500, { skipSeq: 1 }).x(2, 26900);

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
    const log = start().x(1, 8300).x(1, 8600).x(1, 13600);
    const ghost = log.events.at(-3)!;
    const s = fold(cfg(), log.events, [act({ type: 'ignoreCrossing', crossing: { bootId: ghost.bootId, evSeq: ghost.evSeq } })]);
    assert.equal(car(s, 1).reactionMs, 600);
    assert.deepEqual(car(s, 1).lapTimes.map((l) => l.ms), [5000]);
    assert.deepEqual(car(s, 1).rejected.map((r) => r.reason), ['ignored']);
    // a hand in the beam before lights out, ignored by the marshal: no false start
    const fs = start().x(1, 7900).x(1, 8600).x(1, 13600);
    const hand = fs.events.at(-3)!;
    const f = fold(cfg(), fs.events, [act({ type: 'ignoreCrossing', crossing: { bootId: hand.bootId, evSeq: hand.evSeq } })]);
    assert.equal(car(f, 1).falseStart, false);
    assert.equal(car(f, 1).reactionMs, 600);
    assert.deepEqual(car(f, 1).lapTimes.map((l) => l.ms), [5000]);
  });

  test('an ignore/accept naming no crossing is ignored with a reason, not applied', () => {
    const log = start().laps(1, 8500, 5000, 3);
    const lightsOut = log.events[2]!;
    const missing = act({ type: 'ignoreCrossing', crossing: { bootId: 0xb001, evSeq: 999 } });
    const notCrossing = act({ type: 'acceptCrossing', crossing: { bootId: lightsOut.bootId, evSeq: lightsOut.evSeq } });
    const s = fold(cfg({ laps: 10 }), log.events, [missing, notCrossing]);
    assert.deepEqual(s.appliedActions, []);
    assert.deepEqual(s.ignoredActions.map((x) => x.id), [missing.id, notCrossing.id]);
    assert.equal(car(s, 1).laps, 2);
  });

  test('a skipped BLOCKED report still hands its lane outage to the next lap', () => {
    const log = start().x(1, 8500).lane(1, LaneState.LEFT, 9000).lane(1, LaneState.JOINED, 9100)
      .x(1, 13500, { type: LaneEventType.BLOCKED }).x(1, 18500).events;
    const s = fold(cfg({ blockedCountsAsCrossing: false }), log);
    assert.deepEqual(car(s, 1).lapTimes.map((l) => [l.ms, l.flags]), [[10000, ['laneOutage']]]);
  });

  test('a car still on the grid ranks after a started car with no lap yet', () => {
    const s = fold(cfg(), start().x(2, 8600).events);
    assert.deepEqual(s.standings.map((x) => x.lane), [2, 1]);
  });

  test('equal totals tie exactly (µs arithmetic, no float noise), then by lane', () => {
    // A: last crossing at race 16084.010 ms + 300 ms penalty; B: 16384.010 ms
    const log = start().x(1, 8500).x(1, 16000).x(1, 24084.01).x(2, 8600).x(2, 16100).x(2, 24384.01);
    const s = fold(cfg({ laps: 10 }), log.events, [act({ type: 'penalty', lane: 1, ms: 300 })]);
    assert.equal(car(s, 1).totalMs, 16384.01);
    assert.equal(car(s, 2).totalMs, 16384.01);
    assert.deepEqual(s.standings.map((x) => x.lane), [1, 2]);
    assert.deepEqual(s.standings[1]?.gap, { laps: 0, ms: 0 });
  });
});
