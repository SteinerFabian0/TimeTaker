/*
 * Is this crossing a lap? (SYSTEM_ARCHITECTURE §2.2: rules belong in the host.)
 *
 * In this order:
 *   1. ignoreCrossing (host action) → rejected 'ignored'.
 *   2. acceptCrossing (host action) → counted, flagged 'accepted'; no checks.
 *   3. IN_HOLDOFF flag and rejectInHoldoff → rejected 'holdoff'.
 *   4. Less than minLapMs since the car's previous counted crossing (same
 *      boot) → rejected 'minLap'. Across a reboot there is no common clock:
 *      the lap counts with no time.
 * A rejected crossing never moves the car's lap start: the next valid lap is
 * measured from the last counted crossing (a re-slotted car's real lap
 * includes the time it lost).
 *
 * Lap flags (the lap counts, the time deserves a look):
 *   suspectTiming  the event carries one of config.suspectFlags
 *   eventsLost     laneSeq jumped since the lane's previous event (same boot)
 *   laneOutage     the lane LEFT the bus during this lap
 *   rebootGap      the lap spans a control module reboot (time unknown)
 *   blocked        the crossing was a BLOCKED report (car stopped on the line)
 */

import { LaneEventFlag, LaneEventType } from '../../protocol/generated.ts';
import type { LapFlag, RaceConfig, RejectReason } from '../types.ts';
import type { Crossing } from './timeline.ts';

export interface PrevCrossing {
  readonly bootId: number;
  readonly tUs: number;
}

export type LapVerdict =
  | { readonly ok: true; readonly ms: number | null; readonly flags: LapFlag[] }
  | { readonly ok: false; readonly reason: RejectReason; readonly sinceMs: number | null };

export interface LapContext {
  readonly ignored: boolean;
  readonly accepted: boolean;
  readonly eventsLost: boolean;
  readonly laneOutage: boolean;
}

export function judgeLap(config: RaceConfig, c: Crossing, prev: PrevCrossing, ctx: LapContext): LapVerdict {
  const sameBoot = prev.bootId === c.bootId;
  const sinceMs = sameBoot ? (c.tUs - prev.tUs) / 1000 : null;
  if (ctx.ignored) return { ok: false, reason: 'ignored', sinceMs };
  const flags: LapFlag[] = [];
  if (!ctx.accepted) {
    if (config.rejectInHoldoff && (c.flags & LaneEventFlag.IN_HOLDOFF)) return { ok: false, reason: 'holdoff', sinceMs };
    if (sinceMs !== null && sinceMs < config.minLapMs) return { ok: false, reason: 'minLap', sinceMs };
  } else {
    flags.push('accepted');
  }
  if (c.flags & config.suspectFlags) flags.push('suspectTiming');
  if (ctx.eventsLost) flags.push('eventsLost');
  if (ctx.laneOutage) flags.push('laneOutage');
  if (!sameBoot) flags.push('rebootGap');
  if (c.type === LaneEventType.BLOCKED) flags.push('blocked');
  return { ok: true, ms: sinceMs, flags };
}
