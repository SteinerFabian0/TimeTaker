/*
 * The race director: what a UI button means, without the UI.
 *
 *   device intents  → DeviceApi requests (power, prepare lanes, start/abort
 *                     the light sequence, calibrate, LED test, holdoff)
 *   corrections     → host actions appended to the race in the EventStore
 *                     (+/- lap, penalty, disqualify, retire, ignore/accept a
 *                     crossing, end the race, undo)
 *   state           → raceEngine.fold over the store's race input
 *
 * The director never decides results: the device reports facts, the engine
 * folds them. Every correction records `at`, the last device event the app
 * had (a log position, not a host clock), and the wall time for the audit
 * trail only.
 */

import { ALL_LANES, laneMask } from '../device/deviceApi.ts';
import type { DeviceApi, StartParams } from '../device/deviceApi.ts';
import { LedMode, SamplingMode } from '../protocol/generated.ts';
import { fold } from '../race/raceEngine.ts';
import type { HostAction, LogPos, RaceState } from '../race/types.ts';
import type { ActionDraft, EventStore } from '../store/eventStore.ts';

/** The DeviceApi methods the director uses (a fake can stand in). */
export type DirectorApi = Pick<DeviceApi, 'lanePower' | 'laneSampling' | 'laneLed' | 'laneCalibrate' | 'startSequence'
  | 'abortSequence' | 'laneConfigGet' | 'laneConfigSet' | 'status'>;

export const DEFAULT_START: StartParams = { lights: 6, stepMs: 1000, holdMinMs: 200, holdMaxMs: 3000, armSampling: true, leadMs: 0 };

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

  /** The light sequence. Returns T0 (device µs). Refused unless the race is waiting. */
  async start(params: Partial<StartParams> = {}): Promise<bigint> {
    const phase = this.state().phase;
    if (phase !== 'waiting') throw new Error(`cannot start: race is ${phase}`);
    return this.#o.api.startSequence({ ...DEFAULT_START, ...params });
  }

  async abortStart(): Promise<void> {
    await this.#o.api.abortSequence();
  }

  async calibrate(lanes: readonly number[] = this.#lanes()): Promise<void> {
    await this.#o.api.laneCalibrate(laneMask(lanes));
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

  #act(draft: Omit<ActionDraft, 'at'>): HostAction {
    const d = { ...draft, at: this.#o.store.position(), wallTime: this.#o.wallClock?.() ?? new Date().toISOString() } as ActionDraft;
    return this.#o.store.appendAction(this.#o.raceId, d, this.#o.newId);
  }

  adjustLaps(lane: number, delta: number, note?: string): HostAction { return this.#act({ type: 'adjustLaps', lane, delta, note }); }
  penalty(lane: number, ms: number, note?: string): HostAction { return this.#act({ type: 'penalty', lane, ms, note }); }
  disqualify(lane: number, note?: string): HostAction { return this.#act({ type: 'disqualify', lane, note }); }
  retire(lane: number, note?: string): HostAction { return this.#act({ type: 'retire', lane, note }); }
  ignoreCrossing(crossing: LogPos, note?: string): HostAction { return this.#act({ type: 'ignoreCrossing', crossing, note }); }
  acceptCrossing(crossing: LogPos, note?: string): HostAction { return this.#act({ type: 'acceptCrossing', crossing, note }); }
  endRace(note?: string): HostAction { return this.#act({ type: 'endRace', note }); }
  undo(actionId: string, note?: string): HostAction { return this.#act({ type: 'undo', target: actionId, note }); }
}
