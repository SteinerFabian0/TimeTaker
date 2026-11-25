/*
 * Simulated slot cars for SimDevice: when each car's nose crosses the line.
 *
 * Physics-free and deliberately simple. Each car has a pace, a spread, a
 * reaction, and probabilities for the incidents the race rules must handle:
 *
 *   falseStart  jumps during the light sequence and crosses before lights out
 *   deslot      comes off mid-lap; the lap gets 2–6 s longer
 *   double      deslots just after the line and the marshal puts it back
 *               *behind* the line: it crosses again 1.5–3.5 s later (a lap
 *               that must be rejected by the minimum-lap rule)
 *   block       stops on the line (the lane reports BLOCKED, then CLEARED)
 *   merged      a body/wing gap the lane merges (MERGED flag, still one car)
 *   crash       (scripted only) the car crashes out after a crossing and
 *               never crosses again (crash policy "crash = DNF")
 *
 * Paces: SimDevice's own default pace is a short bench lap (4.8–6.8 s, quick
 * protocol tests). Races use raceCarProfiles(): realistic 30–40 s laps with
 * a pace, a consistency and a reaction per driver (RACE_RULES.md: the
 * minimum lap is 10 s, so only a re-slotted "double" is ever that short).
 *
 * The grid is *behind* the line: after lights out a car needs its reaction
 * plus a run-up before its first crossing. That first crossing is what the
 * race engine's "first crossing starts lap 1 timing" rule is about.
 *
 * Times are scheduler milliseconds. SimDevice maps them to sysUs of the
 * current boot. Every car has its own random stream, so link traffic,
 * reconnects or other lanes never change a car's crossing times.
 */

import { makeRng, subSeed } from '../../util/rng.ts';
import type { Rng } from '../../util/rng.ts';
import type { Scheduler, TimerHandle } from '../../util/clock.ts';

export interface CarProfile {
  /** Mean lap time. */
  lapMs: number;
  /** Lap-to-lap spread (one standard deviation). */
  lapSdMs: number;
  /** Reaction to lights out. */
  reactionMs: number;
  reactionSdMs: number;
  /** Grid to the line once moving. */
  runUpMs: number;
  /** Per start. */
  pFalseStart: number;
  /** Per lap. */
  pDeslot: number;
  pDouble: number;
  pBlock: number;
  /** Per crossing. */
  pMerged: number;
  /** Time under the sensor, ms (uniform range). */
  dwellMs: readonly [number, number];
  /** Clean-lap limits (driver variation stays inside); default: 0.8 × lapMs, no upper limit. */
  lapRangeMs?: readonly [number, number];
}

export const DEFAULT_CAR: CarProfile = {
  lapMs: 5500, lapSdMs: 120, reactionMs: 260, reactionSdMs: 60, runUpMs: 450,
  pFalseStart: 0, pDeslot: 0.03, pDouble: 0, pBlock: 0, pMerged: 0.02, dwellMs: [15, 30],
};

/**
 * Race drivers (deterministic per seed and lane): mean lap 32–37 s, lap-to-lap
 * spread 0.3–0.9 s, clean laps kept within 30–40 s; reaction 200–350 ms and a
 * 0.6–0.9 s run-up from the grid behind the line. Pass as SimOptions.cars.
 */
export function raceCarProfiles(seed: number, lanes: readonly number[] = [1, 2, 3, 4, 5, 6]): Record<number, Partial<CarProfile>> {
  const out: Record<number, Partial<CarProfile>> = {};
  for (const lane of lanes) {
    const r = makeRng(subSeed(seed, 300 + lane));
    out[lane] = {
      lapMs: Math.round(r.range(32_000, 37_000)), lapSdMs: Math.round(r.range(300, 900)), lapRangeMs: [30_000, 40_000],
      reactionMs: Math.round(r.range(200, 350)), reactionSdMs: 50, runUpMs: Math.round(r.range(600, 900)),
    };
  }
  return out;
}

export type CrossingKind = 'start' | 'falseStart' | 'lap' | 'double' | 'blocked';

/** A scripted incident: applies right after the car's n-th genuine crossing of this run (1-based). */
export interface CarIncident {
  readonly afterCrossing: number;
  readonly kind: 'deslot' | 'double' | 'block' | 'crash';
}

export interface CarScript {
  /** Force (true) or forbid (false) a false start at the next start sequence. */
  falseStart?: boolean;
  incidents?: readonly CarIncident[];
}

export interface CrossingDetail {
  readonly dwellMs: number;
  /** Signal depth relative to the baseline, lane units. */
  readonly depth: number;
  readonly merged: boolean;
  /** For 'blocked': how long the car stands on the line. */
  readonly blockMs: number;
}

export interface CarHost {
  readonly scheduler: Scheduler;
  onCross(lane: number, atMs: number, kind: CrossingKind, detail: CrossingDetail): void;
}

export type CarState = 'off' | 'grid' | 'driving' | 'parked';

export class SimCar {
  readonly lane: number;
  profile: CarProfile;
  script: CarScript = {};
  #rng: Rng;
  #host: CarHost;
  #state: CarState = 'off';
  #timer: TimerHandle | null = null;
  #genuine = 0;
  #stopAfter = Number.POSITIVE_INFINITY;
  #falseStarted = false;
  #nextIsBlock = false;

  constructor(lane: number, rng: Rng, host: CarHost, profile: CarProfile = DEFAULT_CAR) {
    this.lane = lane;
    this.#rng = rng;
    this.#host = host;
    this.profile = profile;
  }

  get state(): CarState { return this.#state; }
  get genuineCrossings(): number { return this.#genuine; }

  /** Free practice: lap continuously, starting somewhere on the track. */
  practice(): void {
    this.#reset('driving');
    const now = this.#host.scheduler.now();
    this.#at(now + this.#rng.range(0.15, 1) * this.profile.lapMs, 'lap');
  }

  /** Race: wait on the grid for a start sequence. Park after stopAfter genuine crossings. */
  grid(stopAfter = Number.POSITIVE_INFINITY): void {
    this.#reset('grid');
    this.#stopAfter = stopAfter;
  }

  stop(): void {
    this.#reset('off');
  }

  /** A start sequence began (T0) and the lights go out at toutMs (the device knows it, the drivers do not). */
  sequenceStarted(t0Ms: number, toutMs: number): void {
    if (this.#state !== 'grid') return;
    const jump = this.script.falseStart ?? this.#rng.chance(this.profile.pFalseStart);
    if (!jump) return;
    this.#falseStarted = true;
    this.#state = 'driving';
    const at = t0Ms + this.#rng.range(0.35, 0.95) * (toutMs - t0Ms);
    this.#at(Math.max(at, this.#host.scheduler.now() + 1), 'falseStart');
  }

  lightsOut(toutMs: number): void {
    if (this.#state !== 'grid') return;
    this.#state = 'driving';
    const reaction = Math.max(120, this.#rng.normal(this.profile.reactionMs, this.profile.reactionSdMs));
    this.#at(toutMs + reaction + this.profile.runUpMs, 'start');
  }

  /** The start was aborted: everyone back to the grid. */
  sequenceAborted(): void {
    if (this.#state === 'off' || this.#state === 'parked') return;
    this.#reset('grid');
  }

  #reset(state: CarState): void {
    this.#host.scheduler.clearTimeout(this.#timer);
    this.#timer = null;
    this.#state = state;
    this.#genuine = 0;
    this.#falseStarted = false;
    this.#nextIsBlock = false;
    this.#stopAfter = Number.POSITIVE_INFINITY;
  }

  #at(ms: number, kind: CrossingKind): void {
    this.#host.scheduler.clearTimeout(this.#timer);
    const delay = Math.max(0, ms - this.#host.scheduler.now());
    this.#timer = this.#host.scheduler.setTimeout(() => { this.#timer = null; this.#cross(ms, kind); }, delay);
  }

  #lap(): number {
    const p = this.profile;
    const [lo, hi] = p.lapRangeMs ?? [0.8 * p.lapMs, Number.POSITIVE_INFINITY];
    return Math.min(hi, Math.max(lo, this.#rng.normal(p.lapMs, p.lapSdMs)));
  }

  #cross(atMs: number, scheduled: CrossingKind): void {
    const p = this.profile;
    let kind = scheduled;
    if (kind === 'lap' && this.#nextIsBlock) kind = 'blocked';
    this.#nextIsBlock = false;

    const detail: CrossingDetail = {
      dwellMs: this.#rng.range(p.dwellMs[0], p.dwellMs[1]),
      depth: Math.round(this.#rng.range(550, 950)),
      merged: this.#rng.chance(p.pMerged),
      blockMs: kind === 'blocked' ? this.#rng.range(1200, 3000) : 0,
    };
    this.#host.onCross(this.lane, atMs, kind, detail);

    if (kind === 'double') {
      this.#at(atMs + this.#lap(), 'lap');
      return;
    }
    this.#genuine++;
    if (this.#genuine >= this.#stopAfter) {
      this.#state = 'parked';
      return;
    }

    const scripted = this.script.incidents?.filter((i) => i.afterCrossing === this.#genuine).map((i) => i.kind) ?? [];
    const random = this.script.incidents ? [] : [
      ...(this.#rng.chance(p.pDouble) ? ['double' as const] : []),
      ...(this.#rng.chance(p.pDeslot) ? ['deslot' as const] : []),
      ...(this.#rng.chance(p.pBlock) ? ['block' as const] : []),
    ];
    const incidents = new Set([...scripted, ...random]);
    if (incidents.has('crash')) {
      this.#state = 'parked';
      return;
    }

    let next = atMs + detail.blockMs + this.#lap();
    if (incidents.has('deslot')) next += this.#rng.range(2000, 6000);
    if (incidents.has('block')) this.#nextIsBlock = true;
    if (incidents.has('double')) {
      this.#at(atMs + this.#rng.range(1500, 3500), 'double');
      return;
    }
    this.#at(next, 'lap');
  }
}
