// The race running on a Grand Prix race page (GUI_SPEC §6.1b): "Start race"
// plays the start light sequence on the time taker, then every device event
// goes into this race's log and the race engine folds it (src/race). The
// page shows the lights, the lap counts and the lap clocks from that.
//
// Without a time taker the sequence runs here only (a stand-in log with the
// same events), so the page can be tried: the lights go out and the clocks
// run; with the dev tools on, the cars leave the grid by themselves and the
// dev "crossing" buttons count laps. Not stored yet: a reload ends the race (the event
// store comes with the race results).
import { signal, effect } from '@preact/signals';
import { trackLink, raceSessionActive } from '../shell/connection.ts';
import { gpDone, gpLocked, gpStarted, raceKey } from './grandPrix.ts';
import { devTools } from './prefs.ts';
import { fold } from '../../src/race/raceEngine.ts';
import { DEFAULT_START } from '../../src/director/raceDirector.ts';
import { planStartLights, randomHoldMs } from '../../src/track/startLightsPlan.ts';
import { LaneEventType, MsgId } from '../../src/protocol/generated.ts';
import { countCrossing, gpWarmup } from './warmup.ts';
import type { RaceConfig, RaceInputEvent } from '../../src/race/types.ts';

/** Time from pressing "Start race" to T0, so the lanes have their sequence in time (device minimum 150 ms). */
export const LEAD_MS = 300;

export interface RaceRun {
  /** Stage id and race index (0-based) the race belongs to. */
  readonly stage: string;
  readonly race: number;
  readonly config: RaceConfig;
  /** Lights in the start sequence (the track's lanes, 1 … n). */
  readonly lights: number;
  /** T0 of the sequence on this computer's clock (Date.now()). */
  readonly t0Local: number;
  /** The device events since "Start race", in arrival order. */
  readonly events: readonly RaceInputEvent[];
  /** This computer's ms = device µs / 1000 + offsetMs; known from lights out on. */
  readonly offsetMs: number | null;
  /** True when no time taker runs it (the stand-in sequence). */
  readonly local: boolean;
}

export const gpRace = signal<RaceRun | null>(null);
/** False when the time taker refused the start (shown on the page). */
export const raceStartFailed = signal(false);
/**
 * The race whose start lights were stopped before lights out (pressing the
 * button again, or the time taker aborting the sequence): its page offers
 * "Restart start sequence". Cleared by the next start.
 */
export const raceAborted = signal<{ stage: string; race: number } | null>(null);

let unsubscribe: (() => void) | null = null;
let timers: number[] = [];
/** The stand-in's event counter: every stand-in event gets the next one, in the order they happen (like the device's evSeq). */
let localSeq = 0;

function stop(): void {
  unsubscribe?.(); unsubscribe = null;
  timers.forEach(id => clearTimeout(id)); timers = [];
  raceSessionActive.value = false;
}

/** A stand-in event (bootId 0, this computer's clock). */
function appendLocal(msgId: number, name: string, value: Record<string, unknown>): void {
  const evSeq = ++localSeq;
  append({ bootId: 0, evSeq, msgId, name, value: { evSeq, ...value } });
}

const crossingNow = (lane: number) =>
  ({ lane, type: LaneEventType.CROSSING, flags: 0, reserved: 0, tEntryUs: Date.now() * 1000, laneSeq: 0, dwell10us: 0, depth: 0, reserved2: 0 });

function append(e: RaceInputEvent): void {
  const r = gpRace.value;
  if (!r) return;
  let offsetMs = r.offsetMs;
  if (e.msgId === MsgId.EVT_LIGHTS_OUT && e.value && offsetMs === null) {
    offsetMs = Date.now() - Number(e.value['toutUs']) / 1000;
    raceSessionActive.value = false;                 // the sequence is over: the page may set the lights again
  }
  const next = { ...r, events: [...r.events, e], offsetMs };
  gpRace.value = next;
  // A finished race opens the timeline after it.
  const k = raceKey(r.stage, r.race);
  if (!gpDone.value.includes(k) && fold(next.config, next.events).phase === 'finished') gpDone.value = [...gpDone.value, k];
  if (e.msgId === MsgId.EVT_SEQ_ABORTED) { stop(); gpRace.value = null; raceAborted.value = { stage: r.stage, race: r.race }; }
}

/** Starts the light sequence of a race; its events from now on make the race. */
export function beginRace(stage: string, race: number, config: RaceConfig, lights: number): void {
  stop();
  raceStartFailed.value = false;
  raceAborted.value = null;
  const t0Local = Date.now() + LEAD_MS;
  const link = trackLink.value;
  gpRace.value = { stage, race, config, lights, t0Local, events: [], offsetMs: link ? null : 0, local: !link };
  raceSessionActive.value = true;
  const sl = config.startLights;
  if (link) {
    unsubscribe = link.subscribe(e => append(e));
    link.api.startSequence({ ...DEFAULT_START, ...sl, lights, leadMs: LEAD_MS }).catch(() => {
      stop(); gpRace.value = null; raceStartFailed.value = true;
    });
    return;
  }
  // The stand-in: the same events the time taker would send, on this computer's clock (bootId 0).
  localSeq = 0;
  const plan = planStartLights(sl, lights, randomHoldMs(sl));
  const toutLocal = t0Local + plan.outMs;
  appendLocal(MsgId.EVT_SEQ_STARTED, 'EVT_SEQ_STARTED', { lights, armSampling: 1, stepMs: sl.stepMs, t0Us: t0Local * 1000 });
  timers.push(window.setTimeout(() => appendLocal(MsgId.EVT_LIGHTS_OUT, 'EVT_LIGHTS_OUT', { lights, toutUs: toutLocal * 1000 }),
    Math.max(0, toutLocal - Date.now())));
  // With the dev tools on, the cars also leave the grid: each crosses the line once just after lights
  // out (the start crossing, not a lap: RACE_RULES "first crossing is not a lap"), so every
  // "crossing" pressed after that is a lap.
  if (devTools.value) for (const e of config.entries) {
    const reactionMs = 180 + Math.round(Math.random() * 320);
    timers.push(window.setTimeout(() => appendLocal(MsgId.EVT_CROSSING, 'EVT_CROSSING', crossingNow(e.lane)),
      Math.max(0, toutLocal + reactionMs - Date.now())));
  }
}

/**
 * Stops the start lights before lights out (the button pressed again): the
 * time taker aborts its sequence (or the stand-in's timers stop) and the race
 * is gone; the page then offers to restart the sequence from the beginning.
 * Does nothing once the lights are out.
 */
export function abortStart(): void {
  const r = gpRace.value;
  if (!r || r.events.some(e => e.msgId === MsgId.EVT_LIGHTS_OUT)) return;
  const link = r.local ? null : trackLink.value;
  stop();
  gpRace.value = null;
  raceAborted.value = { stage: r.stage, race: r.race };
  void link?.api.abortSequence().catch(() => {});
}

/**
 * Dev tools: this race back to before its warm-up. A running start sequence
 * is aborted, the race and the warm-up are gone, the race no longer counts as
 * finished, and on a stage's first race its lanes may be changed again.
 */
export function resetToPreRace(stage: string, race: number): void {
  const r = gpRace.value;
  if (r?.stage === stage && r.race === race) {
    const starting = !r.local && !r.events.some(e => e.msgId === MsgId.EVT_LIGHTS_OUT);
    stop();
    gpRace.value = null;
    if (starting) void trackLink.value?.api.abortSequence().catch(() => {});
  }
  raceAborted.value = null;
  raceStartFailed.value = false;
  const w = gpWarmup.value;
  if (w?.stage === stage && w.race === race) gpWarmup.value = null;
  const k = raceKey(stage, race);
  if (gpDone.value.includes(k)) gpDone.value = gpDone.value.filter(x => x !== k);
  if (race === 0 && gpLocked.value.includes(stage)) gpLocked.value = gpLocked.value.filter(x => x !== stage);
}

/**
 * Dev tools, without a time taker: a car on `lane` crosses the line now. Goes
 * into the stand-in race's log (on this computer's clock, like its lights),
 * or into the warm-up when no race runs.
 */
export function simulateCrossing(lane: number): void {
  const r = gpRace.value;
  if (r) {
    if (r.local) appendLocal(MsgId.EVT_CROSSING, 'EVT_CROSSING', crossingNow(lane));   // with a time taker only real ones count
    return;
  }
  countCrossing(crossingNow(lane));
}

/** The race of this page, or null when none runs for it. */
export const raceOf = (stage: string, race: number): RaceRun | null => {
  const r = gpRace.value;
  return r && r.stage === stage && r.race === race ? r : null;
};

// A new evening: no race runs.
effect(() => { if (!gpStarted.value) { stop(); gpRace.value = null; raceAborted.value = null; } });
