/*
 * From the raw device event log to what the rules work on.
 *
 * - Boots are ordered by first appearance in the log (bootIds are random).
 *   Within a boot, evSeq is the log order.
 * - The race start: the first EVT_LIGHTS_OUT that follows an EVT_SEQ_STARTED
 *   of the same boot with no EVT_SEQ_ABORTED in between. Aborted sequences
 *   are listed; a later sequence restarts the start procedure. Sequences
 *   after the start are ignored (warning).
 * - Crossings are ordered by tEntryUs within a boot (the lanes timestamp,
 *   the log order across lanes is collection order, not time:
 *   CONTROL guide §5.1 "the host orders by tEntryUs, never by evSeq").
 * - Race time: ms since lights out. A later boot has its own clock; its race
 *   time continues from the last event seen in the boot before (the reboot
 *   downtime is unknown and counted as zero) and is flagged as a timing gap.
 */

import { LaneEventType, LaneState, MsgId } from '../../protocol/generated.ts';
import type { LogPos, RaceInputEvent, RaceStart, TimingGap } from '../types.ts';

export interface Crossing {
  readonly bootId: number;
  readonly ord: number;
  readonly evSeq: number;
  readonly lane: number;
  readonly type: number;
  readonly flags: number;
  readonly tUs: number;
  readonly laneSeq: number;
  /** laneSeq jumped on this lane since its previous counted-type event: events were lost on the lane. */
  readonly lostBefore: boolean;
  /** The lane LEFT the bus since its previous crossing. */
  readonly outageBefore: boolean;
}

export interface LaneChange {
  readonly bootId: number;
  readonly ord: number;
  readonly evSeq: number;
  readonly lane: number;
  readonly state: number;
  readonly tUs: number;
}

export interface Timeline {
  readonly boots: readonly number[];
  readonly start: RaceStart | null;
  readonly aborts: { bootId: number; atUs: number; reason: number }[];
  /** Crossings (CROSSING and BLOCKED) of race interest, in time order. */
  readonly crossings: readonly Crossing[];
  readonly laneChanges: readonly LaneChange[];
  /** Race time (ms) of a device time in a given boot, or null before the start / in an earlier boot. */
  raceMs(bootId: number, tUs: number): number | null;
  readonly timingGaps: readonly TimingGap[];
  readonly lastEvent: LogPos | null;
  readonly lastRaceMs: number | null;
  readonly warnings: string[];
}

const num = (x: unknown): number => (typeof x === 'bigint' ? Number(x) : typeof x === 'number' ? x : 0);

/** The device time an event carries, µs (crossings: tEntryUs). */
export function eventTimeUs(e: RaceInputEvent): number | null {
  const v = e.value;
  if (!v) return null;
  for (const k of ['tEntryUs', 'toutUs', 'atUs', 'sysUs']) if (k in v) return num(v[k]);
  return null;
}

export function buildTimeline(events: readonly RaceInputEvent[], raceLanes: ReadonlySet<number>): Timeline {
  const boots: number[] = [];
  const ordOf = new Map<number, number>();
  for (const e of events) if (!ordOf.has(e.bootId)) { ordOf.set(e.bootId, boots.length); boots.push(e.bootId); }

  // ---- start ----
  let start: RaceStart | null = null;
  let pending: { bootId: number; evSeq: number; lights: number; t0Us: number } | null = null;
  const aborts: { bootId: number; atUs: number; reason: number }[] = [];
  const warnings: string[] = [];
  for (const e of events) {
    const v = e.value;
    if (!v) continue;
    if (start?.toutUs != null) {
      if (e.msgId === MsgId.EVT_SEQ_STARTED) warnings.push(`start sequence at ${e.bootId}/${e.evSeq} after the race start ignored`);
      continue;
    }
    if (e.msgId === MsgId.EVT_SEQ_STARTED) {
      pending = { bootId: e.bootId, evSeq: e.evSeq, lights: v['lights'] as number, t0Us: num(v['t0Us']) };
      start = { ...{ bootId: pending.bootId, seqEvSeq: pending.evSeq, lights: pending.lights, t0Us: pending.t0Us }, toutUs: null };
    } else if (e.msgId === MsgId.EVT_SEQ_ABORTED && pending && e.bootId === pending.bootId) {
      aborts.push({ bootId: e.bootId, atUs: num(v['atUs']), reason: v['reason'] as number });
      pending = null;
      start = null;
    } else if (e.msgId === MsgId.EVT_BOOT && pending && e.bootId !== pending.bootId) {
      warnings.push('the device rebooted during the start sequence');
      pending = null;
      start = null;
    } else if (e.msgId === MsgId.EVT_LIGHTS_OUT && pending && e.bootId === pending.bootId) {
      start = { bootId: pending.bootId, seqEvSeq: pending.evSeq, lights: pending.lights, t0Us: pending.t0Us, toutUs: num(v['toutUs']) };
    }
  }

  // ---- race time per boot ----
  const startOrd = start ? ordOf.get(start.bootId)! : Number.POSITIVE_INFINITY;
  const lastUsByBoot = new Map<number, number>();
  for (const e of events) {
    const t = eventTimeUs(e);
    if (t !== null && e.msgId !== MsgId.EVT_SEQ_STARTED) lastUsByBoot.set(e.bootId, Math.max(lastUsByBoot.get(e.bootId) ?? 0, t));
  }
  const segStartUs = new Map<number, number>();
  const segOffsetMs = new Map<number, number>();
  const timingGaps: TimingGap[] = [];
  if (start?.toutUs != null) {
    let offset = 0;
    let base = start.toutUs;
    for (let o = startOrd; o < boots.length; o++) {
      const b = boots[o]!;
      if (o > startOrd) {
        const prev = boots[o - 1]!;
        offset += Math.max(0, ((lastUsByBoot.get(prev) ?? base) - base) / 1000);
        base = 0;
        timingGaps.push({ fromBootId: prev, toBootId: b, atRaceMs: offset });
      }
      segStartUs.set(b, base);
      segOffsetMs.set(b, offset);
    }
  }
  const raceMs = (bootId: number, tUs: number): number | null => {
    const s = segStartUs.get(bootId);
    if (s === undefined) return null;
    return segOffsetMs.get(bootId)! + (tUs - s) / 1000;
  };

  // ---- crossings and lane changes of interest ----
  const crossings: Crossing[] = [];
  const laneChanges: LaneChange[] = [];
  if (start) {
    // Per lane, in log order (a lane emits in its own order): laneSeq continuity and LEFT.
    const seqState = new Map<number, { bootId: number; last: number | null; lost: boolean; left: boolean }>();
    const laneSt = (lane: number, bootId: number): { bootId: number; last: number | null; lost: boolean; left: boolean } => {
      let st = seqState.get(lane);
      if (!st || st.bootId !== bootId) { st = { bootId, last: null, lost: false, left: st?.left ?? false }; seqState.set(lane, st); }
      return st;
    };
    for (const e of events) {
      const v = e.value;
      if (!v) continue;
      const ord = ordOf.get(e.bootId)!;
      if (ord < startOrd) continue;
      if (e.bootId === start.bootId && e.evSeq === start.seqEvSeq) {
        for (const st of seqState.values()) { st.lost = false; st.left = false; }   // what happened before the start is not the race's
      }
      if (e.msgId === MsgId.EVT_LANE_STATE) {
        const lane = v['lane'] as number;
        const state = v['state'] as number;
        laneChanges.push({ bootId: e.bootId, ord, evSeq: e.evSeq, lane, state, tUs: num(v['sysUs']) });
        const st = laneSt(lane, e.bootId);
        if (state === LaneState.LEFT) st.left = true;
        if (state === LaneState.JOINED) st.last = null;                 // a re-joined lane may have reset its counter
        continue;
      }
      if (e.msgId !== MsgId.EVT_CROSSING) continue;
      const lane = v['lane'] as number;
      const type = v['type'] as number;
      const st = laneSt(lane, e.bootId);
      const laneSeq = v['laneSeq'] as number;
      if (st.last !== null && laneSeq !== ((st.last + 1) & 0xffff)) st.lost = true;
      st.last = laneSeq;
      if (!raceLanes.has(lane) || (type !== LaneEventType.CROSSING && type !== LaneEventType.BLOCKED)) continue;
      const tUs = num(v['tEntryUs']);
      // In the start boot only crossings of the start procedure count: logged after
      // EVT_SEQ_STARTED, or timed at/after T0. Earlier ones are practice.
      if (ord === startOrd && e.evSeq < start.seqEvSeq && tUs < start.t0Us) { st.lost = false; st.left = false; continue; }
      crossings.push({
        bootId: e.bootId, ord, evSeq: e.evSeq, lane, type, flags: v['flags'] as number, tUs, laneSeq,
        lostBefore: st.lost, outageBefore: st.left,
      });
      st.lost = false;
      st.left = false;
    }
  }
  crossings.sort((a, b) => a.ord - b.ord || a.tUs - b.tUs || a.evSeq - b.evSeq);

  const last = events.length ? events[events.length - 1]! : null;
  let lastRaceMs: number | null = null;
  if (start?.toutUs != null && boots.length) {
    const b = boots[boots.length - 1]!;
    const t = lastUsByBoot.get(b);
    if (t !== undefined) lastRaceMs = raceMs(b, t);
  }

  return {
    boots, start, aborts, crossings, laneChanges, raceMs, timingGaps,
    lastEvent: last ? { bootId: last.bootId, evSeq: last.evSeq } : null, lastRaceMs, warnings,
  };
}

/** Log order of two positions, given the boot order. */
export function comparePos(boots: readonly number[], a: LogPos, b: LogPos): number {
  const oa = boots.indexOf(a.bootId);
  const ob = boots.indexOf(b.bootId);
  return (oa === -1 ? Number.POSITIVE_INFINITY : oa) - (ob === -1 ? Number.POSITIVE_INFINITY : ob) || a.evSeq - b.evSeq;
}
