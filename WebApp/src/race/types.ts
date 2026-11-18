/*
 * Race engine types: configuration, host actions, and the folded state.
 *
 * Units: device times are µs on the control module's clock (numbers; sysUs
 * stays far below 2^53). Durations shown to people are ms with µs precision
 * (fractions allowed). Nothing here is ever timestamped by the host.
 */

/** A device event as the engine sees it (DeviceEvent and StoredEvent both fit). */
export interface RaceInputEvent {
  readonly bootId: number;
  readonly evSeq: number;
  readonly msgId: number;
  readonly name?: string;
  readonly value: Record<string, unknown> | null;
}

/** A position in the device event stream: (bootId, evSeq). */
export interface LogPos {
  readonly bootId: number;
  readonly evSeq: number;
}

export interface RaceEntry {
  readonly lane: number;
  readonly driver: string;
  readonly car?: string;
}

export type RaceMode = 'laps' | 'time';

/**
 * How the first crossing after lights out is treated.
 *   startsTiming   the grid is BEHIND the line (the usual layout): the first
 *                  crossing only starts lap timing; lap 1 ends at the second.
 *   completesLap1  the grid is just AFTER the line: lights out starts lap 1
 *                  and the first crossing completes it (lap 1 = c1 − Tout).
 */
export type FirstCrossingRule = 'startsTiming' | 'completesLap1';

export type FalseStartAction = 'flag' | 'penalty' | 'disqualify';

export interface RaceConfig {
  readonly id: string;
  readonly name: string;
  readonly mode: RaceMode;
  /** Laps race: laps to complete. */
  readonly laps: number;
  /** Time race: duration from lights out. */
  readonly durationMs: number;
  readonly entries: readonly RaceEntry[];
  readonly firstCrossing: FirstCrossingRule;
  /** A crossing sooner than this after the car's previous counted crossing is not a lap. */
  readonly minLapMs: number;
  /** A crossing the lane flagged IN_HOLDOFF is not a lap. */
  readonly rejectInHoldoff: boolean;
  /** A BLOCKED report (car stopped on the line) counts as the crossing, at its entry time. */
  readonly blockedCountsAsCrossing: boolean;
  readonly falseStart: {
    readonly action: FalseStartAction;
    /** Added to the race time when action = 'penalty'. */
    readonly penaltyMs: number;
  };
  /** LaneEventFlag bits that make a lap time "suspect" (shown, still counted). */
  readonly suspectFlags: number;
}

// ---- host actions -------------------------------------------------------------

interface ActionBase {
  /** Unique id (undo targets it). */
  readonly id: string;
  /** Order of creation within the race. */
  readonly seq: number;
  /** The last device event the app had when the action was taken (not a host clock). */
  readonly at: LogPos | null;
  readonly note?: string;
  /** Wall clock when it was taken: shown in the audit trail, never used by the rules. */
  readonly wallTime?: string;
}

export type HostAction = ActionBase & (
  | { readonly type: 'adjustLaps'; readonly lane: number; readonly delta: number }
  | { readonly type: 'penalty'; readonly lane: number; readonly ms: number }
  | { readonly type: 'disqualify'; readonly lane: number }
  | { readonly type: 'retire'; readonly lane: number }
  | { readonly type: 'ignoreCrossing'; readonly crossing: LogPos }
  | { readonly type: 'acceptCrossing'; readonly crossing: LogPos }
  | { readonly type: 'endRace' }
  | { readonly type: 'undo'; readonly target: string }
);

export type HostActionType = HostAction['type'];

// ---- state ------------------------------------------------------------------------

export type RacePhase = 'waiting' | 'starting' | 'running' | 'finishing' | 'finished';

export type CarStatus = 'grid' | 'running' | 'finished' | 'disqualified' | 'retired';

export type LapFlag = 'suspectTiming' | 'eventsLost' | 'laneOutage' | 'rebootGap' | 'blocked' | 'accepted';

export type RejectReason = 'minLap' | 'holdoff' | 'ignored' | 'afterFinish' | 'beforeStart';

export interface Lap {
  /** 1-based lap number (adjustments not included). */
  readonly n: number;
  /** null when the lap spans a device reboot (no common clock). */
  readonly ms: number | null;
  readonly endUs: number;
  readonly bootId: number;
  readonly evSeq: number;
  /** Race time at the end of this lap (ms since lights out, across reboots: see timingGaps). */
  readonly raceMs: number;
  readonly flags: readonly LapFlag[];
}

export interface Rejected {
  readonly bootId: number;
  readonly evSeq: number;
  readonly tUs: number;
  readonly reason: RejectReason;
  /** Time since the previous counted crossing, when known. */
  readonly sinceMs: number | null;
}

export interface CarResult {
  readonly lane: number;
  readonly driver: string;
  readonly status: CarStatus;
  /** Counted laps plus manual adjustments. */
  readonly laps: number;
  readonly lapAdjust: number;
  readonly lapTimes: readonly Lap[];
  readonly lastLapMs: number | null;
  readonly bestLapMs: number | null;
  readonly bestLapN: number | null;
  /** Crossing time minus Tout of the start crossing; negative for a false start. */
  readonly reactionMs: number | null;
  readonly falseStart: boolean;
  readonly penaltiesMs: number;
  /** Race time of the last counted crossing (ms since lights out). */
  readonly elapsedMs: number | null;
  /** elapsedMs + penalties: what the ranking uses. */
  readonly totalMs: number | null;
  readonly rejected: readonly Rejected[];
}

export interface Standing {
  readonly position: number;
  readonly lane: number;
  readonly driver: string;
  readonly status: CarStatus;
  readonly laps: number;
  readonly totalMs: number | null;
  /** To the leader: laps behind, or ms when on the same lap. */
  readonly gap: { readonly laps: number; readonly ms: number | null } | null;
  /** To the car ahead, same form. */
  readonly interval: { readonly laps: number; readonly ms: number | null } | null;
  readonly bestLapMs: number | null;
  readonly lastLapMs: number | null;
}

export interface TimingGap {
  readonly fromBootId: number;
  readonly toBootId: number;
  /** Race time when the old boot's last event was seen. */
  readonly atRaceMs: number;
}

export interface RaceStart {
  readonly bootId: number;
  readonly seqEvSeq: number;
  readonly lights: number;
  readonly t0Us: number;
  /** null while the lights are still on. */
  readonly toutUs: number | null;
}

export interface RaceState {
  readonly raceId: string;
  readonly phase: RacePhase;
  readonly start: RaceStart | null;
  readonly aborts: readonly { readonly bootId: number; readonly atUs: number; readonly reason: number }[];
  /** Laps race: the leader took the flag; time race: the time is up. Others finish on their next crossing. */
  readonly flagOut: boolean;
  /** Race time of the latest device event seen (for the clock display; not a result). */
  readonly lastRaceMs: number | null;
  readonly cars: readonly CarResult[];
  readonly standings: readonly Standing[];
  readonly fastestLap: { readonly lane: number; readonly n: number; readonly ms: number } | null;
  readonly timingGaps: readonly TimingGap[];
  readonly lastEvent: LogPos | null;
  readonly appliedActions: readonly string[];
  readonly ignoredActions: readonly { readonly id: string; readonly reason: string }[];
  readonly warnings: readonly string[];
}
