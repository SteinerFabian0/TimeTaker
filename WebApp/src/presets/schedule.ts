/*
 * Who drives on which lane in which race of a group (GUI_SPEC §4.4). Pure
 * functions, DOM-free.
 *
 * A group only uses as many lanes as it has drivers: 5 drivers on a 6-lane
 * track race on 5 lanes and one lane stays empty for the whole group. The
 * lanes swap until every driver has driven every used lane k times (k =
 * the preset's races per lane), so a group always has k × drivers races,
 * however many lanes the track has. Which lanes are used is chosen per group
 * in the Grand Prix (default: the first lanes, so the last ones stay empty;
 * decided 2026-11-23): the host moves a car onto an empty lane, and that
 * driver starts there while his old lane stays empty.
 *
 * Lanes are numbered 1 … L, lane 1 at the start of the chain.
 */

import type { Rotation } from './preset.ts';

/** The lanes a group of `drivers` uses by default: the first ones (lane 1 …), the last stay empty. */
export function defaultActiveLanes(drivers: number, lanes: number): number[] {
  if (!Number.isInteger(drivers) || drivers < 1 || drivers > lanes) return [];
  return Array.from({ length: drivers }, (_, i) => i + 1);
}

/** Where each driver of a group starts (index = driver): lane 1, 2 … by default. */
export function defaultSeating(drivers: number, lanes: number): number[] {
  return defaultActiveLanes(drivers, lanes);
}

/**
 * Moves driver `d`'s car onto the empty lane `to`: he starts there, his old
 * lane becomes empty. Unchanged if `to` is taken or not a lane.
 */
export function moveToLane(seating: readonly number[], d: number, to: number, lanes: number): number[] {
  if (!Number.isInteger(to) || to < 1 || to > lanes || seating.includes(to) || d < 0 || d >= seating.length) return [...seating];
  return seating.map((l, i) => i === d ? to : l);
}

/** A seating is usable: one distinct lane 1 … L per driver. */
export function validSeating(seating: readonly number[], lanes: number): boolean {
  return seating.length >= 1 && new Set(seating).size === seating.length && seating.every(l => Number.isInteger(l) && l >= 1 && l <= lanes);
}

/** The lanes left empty for a group racing on `active`. */
export function inactiveLanes(active: readonly number[], lanes: number): number[] {
  return Array.from({ length: lanes }, (_, i) => i + 1).filter(l => !active.includes(l));
}

export interface ScheduledRace {
  /** 0-based race number within the group. */
  readonly index: number;
  /** Which lane shift this race is: in shift r, driver d sits on active[(d + r) mod n]. */
  readonly shift: number;
  /** One seat per driver, by lane ascending; `driver` is the 0-based index in the group. */
  readonly seats: readonly { readonly lane: number; readonly driver: number }[];
}

/**
 * The races of a group of `drivers` on the lanes `active` (one per driver),
 * k races per lane; driver d starts on the d-th active lane. See raceScheduleSeated.
 */
export function raceSchedule(drivers: number, active: readonly number[], k: number, rotation: Rotation): ScheduledRace[] {
  const lanes = [...active].sort((a, b) => a - b);
  if (!Number.isInteger(drivers) || lanes.length !== drivers) return [];
  return raceScheduleSeated(lanes, k, rotation);
}

/**
 * The races of a group whose driver d starts on lane seating[d], k races per
 * lane. The used lanes are the seated ones; in shift r every driver moves r
 * used lanes on (wrapping round):
 *   block      shift 0 k times, then shift 1 k times, … (the reference evening)
 *   everyRace  shifts 0, 1, … n−1, then again, k times round
 * Every driver drives every used lane exactly k times, and the empty lanes
 * stay empty. Returns [] for an unusable seating.
 */
export function raceScheduleSeated(seating: readonly number[], k: number, rotation: Rotation): ScheduledRace[] {
  const n = seating.length;
  const lanes = [...seating].sort((a, b) => a - b);
  if (n < 1 || new Set(lanes).size !== n || !lanes.every(l => Number.isInteger(l) && l >= 1)
    || !Number.isInteger(k) || k < 1) return [];
  const pos = seating.map(l => lanes.indexOf(l));
  const shifts: number[] = [];
  for (let i = 0; i < k * n; i++) shifts.push(rotation === 'block' ? Math.floor(i / k) : i % n);
  return shifts.map((shift, index) => ({
    index, shift,
    seats: Array.from({ length: n }, (_, d) => ({ lane: lanes[(pos[d] + shift) % n], driver: d }))
      .sort((a, b) => a.lane - b.lane),
  }));
}
