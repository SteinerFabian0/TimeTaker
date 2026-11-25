/*
 * How many lanes the track has (GUI_SPEC §8.0): 2, 4 or 6. Set in Track
 * Setup; every page that plans races (tournament preview, start lights
 * preview, preset summary) uses it. A connected time taker caps it: options
 * above the lanes it reports cannot be chosen, and a setting above them
 * drops to the largest one that fits. Pure functions, DOM-free.
 */

export const LANE_OPTIONS = [2, 4, 6] as const;
export type LaneCount = typeof LANE_OPTIONS[number];
export const DEFAULT_LANES: LaneCount = 6;

/** The options a time taker reporting `reported` lanes allows (all without one; at least the smallest). */
export function allowedLanes(reported: number | null | undefined): LaneCount[] {
  if (!reported || reported < 1) return [...LANE_OPTIONS];
  const ok = LANE_OPTIONS.filter(n => n <= reported);
  return ok.length ? ok : [LANE_OPTIONS[0]];
}

/** The setting as it must be with this time taker: unchanged if allowed, else the largest allowed. */
export function fitLanes(setting: LaneCount, reported: number | null | undefined): LaneCount {
  const ok = allowedLanes(reported);
  return ok.includes(setting) ? setting : ok[ok.length - 1];
}

/** A stored value back to a valid option (the default if it is none). */
export function normalizeLanes(raw: unknown): LaneCount {
  return LANE_OPTIONS.includes(raw as LaneCount) ? raw as LaneCount : DEFAULT_LANES;
}
