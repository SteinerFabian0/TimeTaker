/*
 * Host actions: manual corrections, appended to the race log, never edits.
 *
 * - `undo` cancels its target. An undo can itself be undone (the target is
 *   active again). An undo may only target an earlier action (by seq);
 *   anything else is ignored with a reason, as are actions for lanes not in
 *   the race and duplicate ids.
 * - adjustLaps / penalty / disqualify / retire apply to the whole race (a
 *   "+1 lap" for a missed crossing counts from the start, so the car may
 *   reach the flag one crossing earlier — it did drive that lap).
 * - ignoreCrossing / acceptCrossing override the rules for one crossing,
 *   named by its (bootId, evSeq). An ignored crossing is as if it never
 *   happened (it can be the start crossing or a false start); an accepted one
 *   skips the lap checks. One naming no race crossing in the log is ignored
 *   with a reason.
 * - endRace freezes the race at its `at` position: later crossings do not
 *   count, and the phase becomes finished.
 */

import type { HostAction, LogPos, RaceConfig } from '../types.ts';

export interface ResolvedActions {
  readonly lapAdjust: Map<number, number>;
  readonly penaltyMs: Map<number, number>;
  readonly disqualified: Set<number>;
  readonly retired: Set<number>;
  readonly ignored: Set<string>;
  readonly accepted: Set<string>;
  /** The earliest active endRace position, or 'end' with no position (now). */
  readonly endAt: LogPos | 'now' | null;
  readonly applied: string[];
  readonly rejected: { id: string; reason: string }[];
}

export const posKey = (p: LogPos): string => `${p.bootId >>> 0}:${p.evSeq}`;

export function resolveActions(
  config: RaceConfig, actions: readonly HostAction[], cmp: (a: LogPos, b: LogPos) => number,
  isCrossing: (p: LogPos) => boolean,
): ResolvedActions {
  const lanes = new Set(config.entries.map((e) => e.lane));
  const sorted = [...actions].sort((a, b) => a.seq - b.seq);
  const byId = new Map<string, HostAction>();
  const rejected: { id: string; reason: string }[] = [];
  const valid: HostAction[] = [];
  for (const a of sorted) {
    if (byId.has(a.id)) { rejected.push({ id: a.id, reason: 'duplicate id' }); continue; }
    byId.set(a.id, a);
    if ('lane' in a && !lanes.has(a.lane)) { rejected.push({ id: a.id, reason: `lane ${a.lane} is not in the race` }); continue; }
    if (a.type === 'undo') {
      const target = byId.get(a.target);
      if (!target || target.seq >= a.seq) { rejected.push({ id: a.id, reason: `undo of unknown or later action ${a.target}` }); continue; }
    }
    if (a.type === 'adjustLaps' && !Number.isInteger(a.delta)) { rejected.push({ id: a.id, reason: 'delta must be an integer' }); continue; }
    if (a.type === 'penalty' && !Number.isFinite(a.ms)) { rejected.push({ id: a.id, reason: 'ms must be a number' }); continue; }
    if ((a.type === 'ignoreCrossing' || a.type === 'acceptCrossing') && !isCrossing(a.crossing)) {
      rejected.push({ id: a.id, reason: `no race crossing at ${posKey(a.crossing)}` }); continue;
    }
    valid.push(a);
  }

  // An action is active unless an active undo targets it. Undos only point backwards,
  // so walking from the newest to the oldest settles every undo before its target.
  const active = new Map<string, boolean>();
  const undoneBy = new Map<string, string[]>();
  for (const a of valid) if (a.type === 'undo') undoneBy.set(a.target, [...(undoneBy.get(a.target) ?? []), a.id]);
  for (let i = valid.length - 1; i >= 0; i--) {
    const a = valid[i]!;
    active.set(a.id, !(undoneBy.get(a.id) ?? []).some((u) => active.get(u)));
  }

  const r: ResolvedActions = {
    lapAdjust: new Map(), penaltyMs: new Map(), disqualified: new Set(), retired: new Set(),
    ignored: new Set(), accepted: new Set(), endAt: null, applied: [], rejected,
  };
  let endAt: LogPos | 'now' | null = null;
  for (const a of valid) {
    if (!active.get(a.id)) continue;
    r.applied.push(a.id);
    switch (a.type) {
      case 'adjustLaps': r.lapAdjust.set(a.lane, (r.lapAdjust.get(a.lane) ?? 0) + a.delta); break;
      case 'penalty': r.penaltyMs.set(a.lane, (r.penaltyMs.get(a.lane) ?? 0) + a.ms); break;
      case 'disqualify': r.disqualified.add(a.lane); break;
      case 'retire': r.retired.add(a.lane); break;
      case 'ignoreCrossing': r.ignored.add(posKey(a.crossing)); r.accepted.delete(posKey(a.crossing)); break;
      case 'acceptCrossing': r.accepted.add(posKey(a.crossing)); r.ignored.delete(posKey(a.crossing)); break;
      case 'endRace':
        if (!a.at) endAt = 'now';
        else if (endAt !== 'now' && (endAt === null || cmp(a.at, endAt) < 0)) endAt = a.at;
        break;
      case 'undo': break;
    }
  }
  return { ...r, endAt };
}
