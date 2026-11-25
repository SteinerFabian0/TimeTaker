// The warm-up running on a Grand Prix race page (GUI_SPEC §6.1b): which race,
// and each lane's counted crossings from the time taker. Lives here, not in
// the page, so it keeps counting while the host looks at another tab. Not
// stored: a reload ends it. Rules: src/grandPrix/warmup.ts.
import { signal, computed, effect } from '@preact/signals';
import { trackLink } from '../shell/connection.ts';
import { trackTiming } from './trackTiming.ts';
import { gpLocked, gpStarted } from './grandPrix.ts';
import { addCrossing, startWarmup, type LaneEvent, type WarmupState } from '../../src/grandPrix/warmup.ts';
import { laneMask } from '../../src/track/laneLights.ts';
import { SamplingMode } from '../../src/protocol/generated.ts';

export interface Warmup {
  /** Stage id and race index (0-based) the warm-up belongs to. */
  readonly stage: string;
  readonly race: number;
  /** The lanes with a car. */
  readonly lanes: readonly number[];
  readonly state: WarmupState;
  /** When it started (Date.now()), the cars' random pace, and the nominal drive time on screen. */
  readonly startedAt: number;
  readonly seed: number;
  readonly driveMs: number;
}

export const gpWarmup = signal<Warmup | null>(null);

/** Starts the warm-up of a race: the stage's lanes are fixed from now on, the lanes start detecting. */
export function beginWarmup(stage: string, race: number, lanes: readonly number[], driveMs: number): void {
  gpWarmup.value = {
    stage, race, lanes: [...lanes], state: startWarmup(),
    startedAt: Date.now(), seed: Math.floor(Math.random() * 2 ** 31), driveMs,
  };
  if (!gpLocked.value.includes(stage)) gpLocked.value = [...gpLocked.value, stage];
  void trackLink.value?.api.laneSampling(laneMask(lanes), SamplingMode.ON).catch(() => {});
}

/** The warm-up of this race, or null when none runs for it. */
export const warmupOf = (stage: string, race: number): Warmup | null => {
  const w = gpWarmup.value;
  return w && w.stage === stage && w.race === race ? w : null;
};

// Count the crossings while a warm-up runs and a time taker is connected
// (subscribed once per warm-up, not again for every counted crossing).
const running = computed(() => gpWarmup.value !== null);
effect(() => {
  const link = trackLink.value;
  if (!link || !running.value) return;
  return link.subscribe(e => { if (e.name === 'EVT_CROSSING' && e.value) countCrossing(e.value as unknown as LaneEvent); });
});

/** Counts a lane event into the running warm-up (from the time taker, or the dev tools' stand-in). */
export function countCrossing(e: LaneEvent): void {
  const w = gpWarmup.value;
  if (!w) return;
  const next = addCrossing(w.state, e, w.lanes, trackTiming.value.minLapMs);
  if (next !== w.state) gpWarmup.value = { ...w, state: next };
}

// A new evening (or "Start Grand Prix" not pressed yet): no warm-up runs.
effect(() => { if (!gpStarted.value) gpWarmup.value = null; });
