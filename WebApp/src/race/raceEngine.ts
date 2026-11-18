/*
 * The race engine: a PURE fold (SYSTEM_ARCHITECTURE §2.3)
 *
 *   raceState = fold(config, deviceEvents, hostActions)
 *
 * No IO, no timers, no host clock: the same inputs always give the same
 * state, so a recorded race is a regression test (test/fixtures/races) and
 * any view can rebuild the state from the log at any time.
 *
 * deviceEvents: the device event log in log order (boots in arrival order,
 * evSeq within a boot), from wherever the race was armed onwards. Extra
 * events before the start sequence are harmless (practice is ignored).
 *
 * The rules live in race/rules/ (each file documents its rule):
 *   config      defaults and validation
 *   timeline    boots, the start, crossing order (tEntryUs), race time
 *   start       false starts and reaction times
 *   laps        what counts as a lap, lap flags
 *   finish      the flag (laps and time races)
 *   actions     manual corrections and undo
 *   standings   ranking, gaps, intervals
 */

import { LaneEventType } from '../protocol/generated.ts';
import type {
  CarResult, CarStatus, HostAction, Lap, RaceConfig, RaceInputEvent, RacePhase, RaceState, Rejected,
} from './types.ts';
import { posKey, resolveActions } from './rules/actions.ts';
import { finishesCar, timeIsUp } from './rules/finish.ts';
import { judgeLap } from './rules/laps.ts';
import type { PrevCrossing } from './rules/laps.ts';
import { falseStartPenaltyMs, judgeStart } from './rules/start.ts';
import { rank } from './rules/standings.ts';
import { buildTimeline, comparePos, roundUs } from './rules/timeline.ts';

interface CarWork {
  readonly lane: number;
  readonly driver: string;
  started: boolean;
  hadPreStart: boolean;
  falseStart: boolean;
  reactionMs: number | null;
  prev: PrevCrossing | null;
  laps: Lap[];
  rejected: Rejected[];
  finished: boolean;
  pendingLost: boolean;
  pendingOutage: boolean;
}

export function fold(config: RaceConfig, events: readonly RaceInputEvent[], actions: readonly HostAction[] = []): RaceState {
  const lanes = new Set(config.entries.map((e) => e.lane));
  const tl = buildTimeline(events, lanes);
  const crossingKeys = new Set(tl.crossings.map(posKey));
  const acts = resolveActions(config, actions, (a, b) => comparePos(tl.boots, a, b), (p) => crossingKeys.has(posKey(p)));
  const warnings = [...tl.warnings];

  const work = new Map<number, CarWork>();
  for (const e of config.entries) {
    work.set(e.lane, {
      lane: e.lane, driver: e.driver, started: false, hadPreStart: false, falseStart: false, reactionMs: null,
      prev: null, laps: [], rejected: [], finished: false, pendingLost: false, pendingOutage: false,
    });
  }
  const out = (status: CarStatus): boolean => status === 'disqualified' || status === 'retired';
  const blocked = (lane: number): boolean => acts.disqualified.has(lane) || acts.retired.has(lane);

  let flagOut = false;
  const start = tl.start;
  if (start && start.toutUs !== null) {
    const tout = start.toutUs;
    for (const c of tl.crossings) {
      if (acts.endAt !== null && acts.endAt !== 'now' && comparePos(tl.boots, c, acts.endAt) > 0) continue;
      const car = work.get(c.lane)!;
      car.pendingLost ||= c.lostBefore;                                // kept for the next lap, even from a skipped BLOCKED
      car.pendingOutage ||= c.outageBefore;
      if (c.type !== LaneEventType.CROSSING && !config.blockedCountsAsCrossing) continue;
      const key = posKey(c);
      if (acts.ignored.has(key)) {                                     // as if it never happened: not a start, not a false start
        const v = judgeLap(config, c, car.prev ?? { bootId: -1, tUs: 0 }, { ignored: true, accepted: false, eventsLost: false, laneOutage: false });
        if (!v.ok) {
          car.rejected.push({ bootId: c.bootId, evSeq: c.evSeq, tUs: c.tUs, reason: v.reason, sinceMs: v.sinceMs });
          continue;
        }
      }

      if (c.bootId === start.bootId) {
        const j = judgeStart(config, c.tUs, tout, car.hadPreStart, car.started);
        if (j.kind === 'falseStart') {
          car.falseStart = true;
          car.hadPreStart = true;
          car.reactionMs = j.reactionMs;
          if (j.isStartCrossing) { car.started = true; car.prev = { bootId: c.bootId, tUs: c.tUs }; }
          continue;
        }
        if (j.kind === 'beforeStart') {
          car.rejected.push({ bootId: c.bootId, evSeq: c.evSeq, tUs: c.tUs, reason: 'beforeStart', sinceMs: null });
          continue;
        }
        if (j.kind === 'start') {
          car.reactionMs = j.reactionMs;
          car.started = true;
          if (j.isStartCrossing) { car.prev = { bootId: c.bootId, tUs: c.tUs }; continue; }
          car.prev = { bootId: start.bootId, tUs: tout };              // completesLap1: lap 1 runs from Tout
        }
        if (!car.started) { car.started = true; car.prev = { bootId: start.bootId, tUs: tout }; }
      } else if (!car.started) {
        // First crossing ever, in a boot after the start (the start boot saw none of it).
        car.started = true;
        if (config.firstCrossing === 'startsTiming') { car.prev = { bootId: c.bootId, tUs: c.tUs }; continue; }
        car.prev = { bootId: start.bootId, tUs: tout };
      }

      const prev = car.prev ?? { bootId: start.bootId, tUs: tout };
      if (car.finished) {
        car.rejected.push({
          bootId: c.bootId, evSeq: c.evSeq, tUs: c.tUs, reason: 'afterFinish',
          sinceMs: prev.bootId === c.bootId ? (c.tUs - prev.tUs) / 1000 : null,
        });
        continue;
      }
      const v = judgeLap(config, c, prev, {
        ignored: acts.ignored.has(key), accepted: acts.accepted.has(key),
        eventsLost: car.pendingLost, laneOutage: car.pendingOutage,
      });
      if (!v.ok) {
        car.rejected.push({ bootId: c.bootId, evSeq: c.evSeq, tUs: c.tUs, reason: v.reason, sinceMs: v.sinceMs });
        continue;
      }
      car.pendingLost = false;
      car.pendingOutage = false;
      const raceMs = tl.raceMs(c.bootId, c.tUs) ?? 0;
      car.laps.push({ n: car.laps.length + 1, ms: v.ms, endUs: c.tUs, bootId: c.bootId, evSeq: c.evSeq, raceMs, flags: v.flags });
      car.prev = { bootId: c.bootId, tUs: c.tUs };
      if (blocked(c.lane)) continue;                                   // DQ / retired cars never take the flag
      const total = car.laps.length + (acts.lapAdjust.get(c.lane) ?? 0);
      if (finishesCar(config, flagOut, total, raceMs)) {
        car.finished = true;
        flagOut = true;
      }
    }
  }

  // ---- results -------------------------------------------------------------------
  const cars: CarResult[] = config.entries.map((e) => {
    const w = work.get(e.lane)!;
    const adjust = acts.lapAdjust.get(e.lane) ?? 0;
    const fsDq = w.falseStart && config.falseStart.action === 'disqualify';
    const status: CarStatus = acts.disqualified.has(e.lane) || fsDq ? 'disqualified'
      : acts.retired.has(e.lane) ? 'retired'
      : w.finished ? 'finished'
      : w.started || w.laps.length > 0 ? 'running' : 'grid';
    const penalties = roundUs((acts.penaltyMs.get(e.lane) ?? 0) + (w.falseStart ? falseStartPenaltyMs(config) : 0));
    const timed = w.laps.filter((l) => l.ms !== null);
    const best = timed.reduce<Lap | null>((b, l) => (b === null || l.ms! < b.ms! ? l : b), null);
    const last = w.laps.at(-1) ?? null;
    const elapsedMs = last ? last.raceMs : null;
    const r: CarResult = {
      lane: e.lane, driver: e.driver, status, laps: w.laps.length + adjust, lapAdjust: adjust, lapTimes: w.laps,
      lastLapMs: last?.ms ?? null, bestLapMs: best?.ms ?? null, bestLapN: best?.n ?? null,
      reactionMs: w.reactionMs, falseStart: w.falseStart, penaltiesMs: penalties,
      elapsedMs, totalMs: elapsedMs === null ? null : elapsedMs + penalties, rejected: w.rejected,
    };
    return { ...r, totalMs: r.totalMs === null ? null : roundUs(r.totalMs) };   // whole µs: equal times tie exactly
  });

  let fastestLap: RaceState['fastestLap'] = null;
  for (const c of cars) {
    if (c.status === 'disqualified') continue;
    for (const l of c.lapTimes) if (l.ms !== null && (!fastestLap || l.ms < fastestLap.ms)) fastestLap = { lane: c.lane, n: l.n, ms: l.ms };
  }

  let phase: RacePhase;
  if (!start) phase = 'waiting';
  else if (start.toutUs === null) phase = 'starting';
  else if (acts.endAt !== null) phase = 'finished';
  else {
    const racing = cars.filter((c) => !out(c.status) && c.status !== 'grid');
    if (flagOut && racing.every((c) => c.status === 'finished')) phase = 'finished';
    else if (flagOut || timeIsUp(config, tl.lastRaceMs)) phase = 'finishing';
    else phase = 'running';
  }
  if (acts.endAt !== null && !start?.toutUs) warnings.push('endRace before the start');

  return {
    raceId: config.id, phase, start, aborts: tl.aborts, flagOut: flagOut || timeIsUp(config, tl.lastRaceMs),
    lastRaceMs: tl.lastRaceMs, cars, standings: rank(cars), fastestLap, timingGaps: tl.timingGaps,
    lastEvent: tl.lastEvent, appliedActions: acts.applied, ignoredActions: acts.rejected, warnings,
  };
}
