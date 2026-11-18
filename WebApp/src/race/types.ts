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

/** RACE_RULES.md "False start": what happens. */
export type FalseStartAction = 'flag' | 'timePenalty' | 'disqualify';

/**
 * How a false-start time penalty applies.
 *   addToFirstLap  added to the car's first lap time (and so to its total)
 *   addToTotal     added to the total only
 *   stopAndGo      FUTURE (power board): the car is held for the penalty time.
 *                  Documented here; config validation rejects it for now.
 */
export type PenaltyApplication = 'addToFirstLap' | 'addToTotal' | 'stopAndGo';

/**
 * RACE_RULES.md "Crashes".
 *   replacementAllowed  a marshal puts the car back; the lap is just long
 *   crashIsDnf          the host marks the car crashed/out (crashOut action);
 *                       it leaves the list of cars the race waits for
 */
export type CrashPolicy = 'replacementAllowed' | 'crashIsDnf';

/** The start light sequence (RACE_RULES.md "Start lights"); the director sends it. */
export interface StartLightsConfig {
  /** Off-on-off breathing cycles before the countdown. */
  readonly attentionPulses: number;
  /** Pulse rise time, off to full. */
  readonly pulseRiseMs: number;
  /** One more lane lights every stepMs. */
  readonly stepMs: number;
  /** Random hold with all lights on, then lights out. */
  readonly holdMinMs: number;
  readonly holdMaxMs: number;
}

/**
 * Fixed rules (RACE_RULES.md, not settings): the grid is behind the line (the
 * first crossing after lights out starts lap 1's timing), a BLOCKED report
 * (car stopped on the line) is a crossing, crossings timed before the start
 * sequence began are ignored, lap times use device timestamps only.
 */
export interface RaceConfig {
  readonly id: string;
  readonly name: string;
  readonly mode: RaceMode;
  /** Laps race: laps to complete. */
  readonly laps: number;
  /** Time race: duration from lights out. */
  readonly durationMs: number;
  readonly entries: readonly RaceEntry[];
  /** A crossing sooner than this after the car's previous counted crossing is never a lap. */
  readonly minLapMs: number;
  /** A crossing the lane flagged IN_HOLDOFF is not a lap. */
  readonly rejectInHoldoff: boolean;
  readonly falseStart: {
    readonly action: FalseStartAction;
    /** The time penalty (action 'timePenalty'). */
    readonly penaltyMs: number;
    readonly apply: PenaltyApplication;
  };
  readonly crashPolicy: CrashPolicy;
  readonly startLights: StartLightsConfig;
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
  /** Crash = DNF policy only: the car is out from `at`; the race no longer waits for it. */
  | { readonly type: 'crashOut'; readonly lane: number }
  | { readonly type: 'ignoreCrossing'; readonly crossing: LogPos }
  | { readonly type: 'endRace' }
  | { readonly type: 'undo'; readonly target: string }
);

export type HostActionType = HostAction['type'];

// ---- state ------------------------------------------------------------------------

export type RacePhase = 'waiting' | 'starting' | 'running' | 'finishing' | 'finished';

export type CarStatus = 'grid' | 'running' | 'finished' | 'disqualified' | 'retired' | 'crashed';

/** falseStartPenalty: the false-start time penalty is included in this lap's ms. */
export type LapFlag = 'suspectTiming' | 'eventsLost' | 'laneOutage' | 'rebootGap' | 'blocked' | 'falseStartPenalty';

/** disqualified / out: after the car's disqualification or crash-out position. */
export type RejectReason = 'minLap' | 'holdoff' | 'ignored' | 'afterFinish' | 'beforeStart' | 'disqualified' | 'out';

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
  /** Disqualified with a lap in progress: that lap's number, shown "TLF" (no time). */
  readonly tlfLap: number | null;
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
