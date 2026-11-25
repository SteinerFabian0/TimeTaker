/*
 * The warm-up before a race (GUI_SPEC §6.1b). Pure, DOM-free.
 *
 * The cars stand just before the start/finish line when the host starts the
 * warm-up. Every counted crossing starts the next warm-up lap: the first
 * crossing starts lap 1, crossing n starts lap n. As soon as a car starts
 * its last warm-up lap its light goes red: stop before the line. A crossing
 * after that is one too many; the app only shows it (the host decides).
 *
 * A crossing is counted when it is a real crossing and comes at least the
 * track's minimum lap time after the car's previous counted one (a car that
 * rocks on the line is not two laps).
 */

import { LaneEventType } from '../protocol/generated.ts';

export interface WarmupState {
  /** Counted crossings per lane (lane number as key). */
  readonly counts: Readonly<Record<number, number>>;
  /** Time of each lane's last counted crossing, in µs (the device clock). */
  readonly lastUs: Readonly<Record<number, number>>;
}

export const startWarmup = (): WarmupState => ({ counts: {}, lastUs: {} });

/** A lane event as the time taker reports it (EVT_CROSSING). */
export interface LaneEvent {
  readonly lane: number;
  readonly type: number;
  readonly flags: number;
  readonly tEntryUs: number | bigint;
}

/**
 * The state after one lane event. Unchanged (the same object) when it does
 * not count: not a crossing, a lane not in the race, or too soon after the
 * car's previous counted crossing.
 */
export function addCrossing(s: WarmupState, e: LaneEvent, lanes: readonly number[], minLapMs: number): WarmupState {
  if (e.type !== LaneEventType.CROSSING || !lanes.includes(e.lane)) return s;
  const t = Number(e.tEntryUs);
  const prev = s.lastUs[e.lane];
  if (prev !== undefined && t - prev < minLapMs * 1000) return s;
  return {
    counts: { ...s.counts, [e.lane]: (s.counts[e.lane] ?? 0) + 1 },
    lastUs: { ...s.lastUs, [e.lane]: t },
  };
}

export interface LaneWarmup {
  /** The warm-up lap the car is in (0 = still before the line). */
  readonly lap: number;
  /** In its last warm-up lap (or past it): light red, stop before the line. */
  readonly stop: boolean;
  /** Crossed the line again after it should have stopped. */
  readonly over: boolean;
  /**
   * Show "lap / laps". Not with a single warm-up lap (that lap never ends:
   * the car parks before the line), unless the car crossed again anyway.
   */
  readonly showCount: boolean;
}

export function laneWarmup(crossings: number, warmupLaps: number): LaneWarmup {
  const over = crossings > warmupLaps;
  return { lap: crossings, stop: crossings >= warmupLaps, over, showCount: warmupLaps >= 2 || over };
}

/** The lanes whose light is red now. */
export function stopLanes(s: WarmupState, lanes: readonly number[], warmupLaps: number): number[] {
  return lanes.filter(l => laneWarmup(s.counts[l] ?? 0, warmupLaps).stop);
}

// ---- the warm-up drive on screen (no Power Pack) ---------------------------------------
//
// Without a Power Pack the app cannot see where the cars are, so the race
// page shows an estimate: each car drives from the left edge of its lane to
// just before the line in about 70 % of the warm-up laps at the average lap
// time. Every car gets its own pace (a little faster or slower overall, and
// uneven along the way), so they spread out. With a Power Pack this becomes
// a real progress display (later).

/** Share of the average warm-up that the drive on screen takes (a bit quicker than average). */
export const DRIVE_SHARE = 0.7;
/** How much one car's drive may differ from the others (±). */
export const DRIVE_SPREAD = 0.12;

/** The nominal drive time: average lap × warm-up laps × 0.7. */
export function warmupDriveMs(avgLapMs: number, warmupLaps: number): number {
  return Math.max(0, Math.round(avgLapMs * warmupLaps * DRIVE_SHARE));
}

/** A small deterministic random generator (mulberry32), so a page reload replays the same drive. */
function rng(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export interface CarDrive {
  /** How long this car takes, in ms. */
  readonly durationMs: number;
  /** Progress over time: t and p in 0..1, both rising, from (0,0) to (1,1). */
  readonly points: readonly { readonly t: number; readonly p: number }[];
}

/** One car's drive: its own duration (nominal ± DRIVE_SPREAD) and an uneven pace along the way. */
export function carDrive(seed: number, lane: number, nominalMs: number, steps = 8): CarDrive {
  const r = rng(seed * 31 + lane * 7919);
  const durationMs = Math.round(nominalMs * (1 + (r() * 2 - 1) * DRIVE_SPREAD));
  // Each step gets a speed between 0.6 and 1.4; progress is their running sum.
  const speeds = Array.from({ length: steps }, () => 0.6 + r() * 0.8);
  const total = speeds.reduce((a, b) => a + b, 0);
  const points = [{ t: 0, p: 0 }];
  let sum = 0;
  speeds.forEach((v, i) => {
    sum += v;
    points.push({ t: (i + 1) / steps, p: i === steps - 1 ? 1 : sum / total });
  });
  return { durationMs, points };
}
