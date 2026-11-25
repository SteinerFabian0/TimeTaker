/*
 * The start light sequence as a timeline (SYSTEM_ARCHITECTURE §6.4), for the
 * Track Setup preview. The same plan the control module's sequence_manager
 * runs, in ms from T0, for n lanes (lane 1 is the start of the daisy chain,
 * lane n its end):
 *
 *   breathing   k = attentionPulses cycles, every lane in unison,
 *               level = (1 − cos(π·t/P))/2, peaks at (2i+1)·P
 *   first lane  lane 1 goes ON at the last peak (2k−1)·P (k = 0: at 0);
 *               the others finish the fall and are OFF at 2k·P
 *   countdown   lane 1+j lights at 2k·P + j·step (j = 1..n−1)
 *   hold        all lit at Tlit, lights out at Tout = Tlit + hold
 *
 * Pure: no DOM, no clock.
 */

import type { StartLightsConfig } from '../race/types.ts';

export interface StartLightsPlan {
  readonly lanes: number;
  /** All lights lit. */
  readonly litMs: number;
  /** Lights out: the race would start. */
  readonly outMs: number;
}

export function planStartLights(sl: StartLightsConfig, lanes: number, holdMs: number): StartLightsPlan {
  const n = Math.max(1, Math.min(6, Math.round(lanes)));
  const litMs = 2 * sl.attentionPulses * sl.pulseRiseMs + (n - 1) * sl.stepMs;
  return { lanes: n, litMs, outMs: litMs + Math.max(0, holdMs) };
}

/** A random hold from the configured range (the device draws its own from its TRNG). */
export function randomHoldMs(sl: StartLightsConfig, rnd: () => number = Math.random): number {
  return Math.round(sl.holdMinMs + rnd() * (sl.holdMaxMs - sl.holdMinMs));
}

/** Each lane's light level 0..1 at t ms after T0 (index 0 = lane 1). */
export function startLightLevels(sl: StartLightsConfig, plan: StartLightsPlan, t: number): number[] {
  const { lanes: n } = plan;
  const k = sl.attentionPulses;
  const P = sl.pulseRiseMs;
  const B = 2 * k * P;
  const out: number[] = [];
  for (let lane = 1; lane <= n; lane++) {
    let level = 0;
    if (t < 0 || t >= plan.outMs) level = 0;
    else if (lane === 1 && t >= (k > 0 ? (2 * k - 1) * P : 0)) level = 1;
    else if (t < B) level = (1 - Math.cos(Math.PI * t / P)) / 2;
    else level = t >= B + (lane - 1) * sl.stepMs ? 1 : 0;
    out.push(level);
  }
  return out;
}
