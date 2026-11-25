/*
 * Track timing (GUI_SPEC §8.1): the settings that belong to the track, not
 * to a race preset. Every race on this track uses them, whatever preset it
 * runs. Plain data and pure functions, DOM-free.
 *
 *   start lights    the start light sequence the director sends
 *   lap timing      the minimum lap time (any faster lap is discarded)
 *
 * Defaults are the race engine's (RACE_RULES.md). Stored with schemaVersion;
 * normalizeTrackTiming() reads any stored shape back into a valid one.
 */

import { DEFAULT_RACE, DEFAULT_START_LIGHTS } from '../race/rules/config.ts';
import type { RaceConfig, StartLightsConfig } from '../race/types.ts';

export const TRACK_TIMING_SCHEMA_VERSION = 1;

export interface TrackTiming {
  readonly schemaVersion: number;
  readonly startLights: StartLightsConfig;
  /** A crossing sooner than this after the car's previous counted one is never a lap. */
  readonly minLapMs: number;
}

export const DEFAULT_TRACK_TIMING: TrackTiming = {
  schemaVersion: TRACK_TIMING_SCHEMA_VERSION,
  startLights: DEFAULT_START_LIGHTS,
  minLapMs: DEFAULT_RACE.minLapMs,
};

/** Limits in ms (the editor shows seconds). */
export const TIMING_LIMITS = {
  attentionPulses: { min: 0, max: 99 },
  pulseRiseMs: { min: 100, max: 1000 },
  stepMs: { min: 100, max: 2000 },
  holdMs: { min: 0, max: 10_000 },
  minLapMs: { min: 2_000, max: 99_000 },
} as const;

export type TimingProblem = 'startLights' | 'minLap';

const intIn = (x: unknown, lim: { min: number; max: number }): boolean =>
  Number.isInteger(x) && (x as number) >= lim.min && (x as number) <= lim.max;

export function validateTrackTiming(t: TrackTiming): TimingProblem[] {
  const out: TimingProblem[] = [];
  const sl = t.startLights;
  if (!intIn(sl.attentionPulses, TIMING_LIMITS.attentionPulses) || !intIn(sl.pulseRiseMs, TIMING_LIMITS.pulseRiseMs)
    || !intIn(sl.stepMs, TIMING_LIMITS.stepMs) || !intIn(sl.holdMinMs, TIMING_LIMITS.holdMs)
    || !intIn(sl.holdMaxMs, TIMING_LIMITS.holdMs) || sl.holdMinMs > sl.holdMaxMs) out.push('startLights');
  if (!intIn(t.minLapMs, TIMING_LIMITS.minLapMs)) out.push('minLap');
  return out;
}

const num = (v: unknown, lim: { min: number; max: number }, fallback: number): number =>
  typeof v === 'number' && Number.isFinite(v) ? Math.round(Math.min(lim.max, Math.max(lim.min, v))) : fallback;

/** Missing or broken fields take the default or are clamped; never throws. */
export function normalizeTrackTiming(raw: unknown): TrackTiming {
  const r = (raw && typeof raw === 'object') ? raw as Record<string, any> : {};
  const sl = (r.startLights && typeof r.startLights === 'object') ? r.startLights : {};
  const d = DEFAULT_TRACK_TIMING;
  const holdMinMs = num(sl.holdMinMs, TIMING_LIMITS.holdMs, d.startLights.holdMinMs);
  return {
    schemaVersion: TRACK_TIMING_SCHEMA_VERSION,
    startLights: {
      attentionPulses: num(sl.attentionPulses, TIMING_LIMITS.attentionPulses, d.startLights.attentionPulses),
      pulseRiseMs: num(sl.pulseRiseMs, TIMING_LIMITS.pulseRiseMs, d.startLights.pulseRiseMs),
      stepMs: num(sl.stepMs, TIMING_LIMITS.stepMs, d.startLights.stepMs),
      holdMinMs,
      holdMaxMs: Math.max(holdMinMs, num(sl.holdMaxMs, TIMING_LIMITS.holdMs, d.startLights.holdMaxMs)),
    },
    minLapMs: num(r.minLapMs, TIMING_LIMITS.minLapMs, d.minLapMs),
  };
}

export function isDefaultTrackTiming(t: TrackTiming): boolean {
  return JSON.stringify(normalizeTrackTiming(t)) === JSON.stringify(DEFAULT_TRACK_TIMING);
}

/** The part of a race's config that comes from the track. */
export function trackRaceConfig(t: TrackTiming): Pick<RaceConfig, 'minLapMs' | 'startLights'> {
  return { minLapMs: t.minLapMs, startLights: { ...t.startLights } };
}
