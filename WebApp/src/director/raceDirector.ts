/*
 * The race director: what a UI button means, without the UI.
 *
 *   device intents  → DeviceApi requests (power, prepare lanes, start/abort
 *                     the light sequence, calibrate, LED test, holdoff)
 *   corrections     → host actions appended to the race in the EventStore
 *                     (+/- lap, penalty, disqualify, retire, crashed/out,
 *                     ignore a crossing, end the race, undo). There is no
 *                     "accept crossing": a lap under the minimum never counts
 *                     (RACE_RULES.md).
 *   state           → raceEngine.fold over the store's race input
 *
 * The director never decides results: the device reports facts, the engine
 * folds them. Every correction records `at`, the last device event the app
 * had (a log position, not a host clock), and the wall time for the audit
 * trail only.
 */

import { ALL_LANES, laneMask } from '../device/deviceApi.ts';
import type { DeviceApi, StartParams } from '../device/deviceApi.ts';
import { CalibrateAction, LedMode, SamplingMode } from '../protocol/generated.ts';
import { fold } from '../race/raceEngine.ts';
import { DEFAULT_START_LIGHTS } from '../race/rules/config.ts';
import type { HostAction, LogPos, RaceState } from '../race/types.ts';
import type { ActionDraft, EventStore } from '../store/eventStore.ts';

/** The DeviceApi methods the director uses (a fake can stand in). */
export type DirectorApi = Pick<DeviceApi, 'lanePower' | 'laneSampling' | 'laneLed' | 'laneCalibrate' | 'startSequence'
  | 'abortSequence' | 'laneConfigGet' | 'laneConfigSet' | 'status'>;

/**
 * The start light sequence (RACE_RULES.md "Start lights"): 2 attention pulses,
 * rise 1000 ms, a lane every 1000 ms, hold 200–3000 ms. A race's own
 * config.startLights replaces these; start() params override both.
 */
export const DEFAULT_START: StartParams = {
  lights: 6, armSampling: true, leadMs: 0,
  attentionPulses: DEFAULT_START_LIGHTS.attentionPulses, pulseRiseMs: DEFAULT_START_LIGHTS.pulseRiseMs,
  stepMs: DEFAULT_START_LIGHTS.stepMs, holdMinMs: DEFAULT_START_LIGHTS.holdMinMs, holdMaxMs: DEFAULT_START_LIGHTS.holdMaxMs,
};

export interface DirectorOptions {
  readonly api: DirectorApi;
  readonly store: EventStore;
  readonly raceId: string;
  readonly newId?: () => string;
  readonly wallClock?: () => string;
}

/**
 * Holdoff for the lanes from the fastest valid lap so far (SYSTEM_ARCHITECTURE
 * §2.4): factor × fastest lap, 0 until there is one. The lane only flags
 * events inside it; the engine decides (rejectInHoldoff).
 */
export function adaptiveHoldoffUs(state: RaceState, factor = 0.55): number {
  if (!state.fastestLap) return 0;
  return Math.floor(state.fastestLap.ms * factor) * 1000;
}

export class RaceDirector {
  #o: DirectorOptions;

  constructor(o: DirectorOptions) {
    this.#o = o;
  }

  get raceId(): string { return this.#o.raceId; }

  state(): RaceState {
    const input = this.#o.store.raceInput(this.#o.raceId);
    return fold(input.config, input.events, input.actions);
  }

  #lanes(): number[] {
    return this.#o.store.race(this.#o.raceId).config.entries.map((e) => e.lane);
  }

  // ---- device intents -------------------------------------------------------------

  async powerLanes(on: boolean): Promise<void> {
    await this.#o.api.lanePower(on);
  }

  /** Race lanes sampling, start lights off. Power must be on and the lanes present. */
  async prepareLanes(): Promise<void> {
    const mask = laneMask(this.#lanes());
    await this.#o.api.laneSampling(mask, SamplingMode.ON);
    await this.#o.api.laneLed(mask, { mode: LedMode.OFF });
  }

  /**
   * The light sequence with the race's startLights settings. Returns T0 (device
   * µs). Refused unless the race is waiting. The host presses start only when
   * no car is moving (RACE_RULES.md); crossings before T0 are ignored anyway.
   */
  async start(params: Partial<StartParams> = {}): Promise<bigint> {
    const phase = this.state().phase;
    if (phase !== 'waiting') throw new Error(`cannot start: race is ${phase}`);
    return this.#o.api.startSequence({ ...this.startParams(), ...params });
  }

  /** What start() sends: DEFAULT_START with the race's startLights. */
  startParams(): StartParams {
    const sl = this.#o.store.race(this.#o.raceId).config.startLights ?? DEFAULT_START_LIGHTS;
    return { ...DEFAULT_START, ...sl };
  }

  async abortStart(): Promise<void> {
    await this.#o.api.abortSequence();
  }

  /**
   * Fire-and-forget LEARN of the race lanes (they must be free). Refused while the race is starting or running.
   * The outcome arrives as EVT_LANE_CALIBRATED; HealthMonitor.calibrate() waits for it and is what a UI uses.
   */
  async calibrate(lanes: readonly number[] = this.#lanes()): Promise<void> {
    if (this.active()) throw new Error(`cannot calibrate: race is ${this.state().phase}`);
    await this.#o.api.laneCalibrate(laneMask(lanes), CalibrateAction.LEARN);
  }

  /** The start sequence or the race is under way (starting, running, finishing): no calibration, slow diagnostics. */
  active(): boolean {
    const p = this.state().phase;
    return p === 'starting' || p === 'running' || p === 'finishing';
  }

  /** Blinks every lane's light three times (wiring check). */
  async ledTest(): Promise<void> {
    await this.#o.api.laneLed(ALL_LANES, { mode: LedMode.BLINK, level: 1000, blinkPeriodMs: 400, blinkOnPermille: 500, blinkCount: 3 });
  }

  /** Sends the adaptive holdoff to the race lanes; returns it (µs). */
  async applyAdaptiveHoldoff(factor = 0.55): Promise<number> {
    const holdoffUs = adaptiveHoldoffUs(this.state(), factor);
    for (const lane of this.#lanes()) {
      const cfg = await this.#o.api.laneConfigGet(lane);
      if (cfg.holdoffUs !== holdoffUs) await this.#o.api.laneConfigSet(laneMask([lane]), { ...cfg, holdoffUs }, false);
    }
    return holdoffUs;
  }

  // ---- corrections (host actions) ----------------------------------------------------

  #act(draft: ActionDraft extends infer A ? A extends ActionDraft ? Omit<A, 'at'> : never : never): HostAction {
    const d = { ...draft, at: this.#o.store.position(), wallTime: this.#o.wallClock?.() ?? new Date().toISOString() } as ActionDraft;
    return this.#o.store.appendAction(this.#o.raceId, d, this.#o.newId);
  }

  adjustLaps(lane: number, delta: number, note?: string): HostAction { return this.#act({ type: 'adjustLaps', lane, delta, note }); }
  penalty(lane: number, ms: number, note?: string): HostAction { return this.#act({ type: 'penalty', lane, ms, note }); }
  /** From now (the last device event): later laps never count, the lap in progress is TLF. */
  disqualify(lane: number, note?: string): HostAction { return this.#act({ type: 'disqualify', lane, note }); }
  retire(lane: number, note?: string): HostAction { return this.#act({ type: 'retire', lane, note }); }
  /** "Crashed / out" (crash policy crashIsDnf): the race stops waiting for this car. */
  crashOut(lane: number, note?: string): HostAction {
    if (this.#o.store.race(this.#o.raceId).config.crashPolicy !== 'crashIsDnf') {
      throw new Error('crash policy is replacementAllowed: a crashed car is put back, nothing to enter');
    }
    return this.#act({ type: 'crashOut', lane, note });
  }
  ignoreCrossing(crossing: LogPos, note?: string): HostAction { return this.#act({ type: 'ignoreCrossing', crossing, note }); }
  endRace(note?: string): HostAction { return this.#act({ type: 'endRace', note }); }
  undo(actionId: string, note?: string): HostAction { return this.#act({ type: 'undo', target: actionId, note }); }
}
