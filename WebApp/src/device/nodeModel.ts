/*
 * The node model: what is attached to the control module right now, built
 * from NODE_LIST answers plus the events that change it (SYSTEM_ARCHITECTURE
 * §7.5: "the web app builds its UI from it").
 *
 * Pure and immutable: applyNodeList / applyNodeEvent return a new model.
 *
 *   EVT_BOOT        the device rebooted: lanes gone (the rail is off after
 *                   every boot), seq not running, BLE link unknown
 *   EVT_LANE_STATE  JOINED / CONFIGURED / LEFT / FAULT / SYNC_LOST / SYNC_OK
 *   EVT_BLE_LINK    the BLE link to a central
 *   EVT_SEQ_*       whether a start sequence runs
 *   NODE_LIST       versions and board types (lanes listed are present; a
 *                   lane the model had as absent becomes 'joined' if listed
 *                   UPDATING (config push running), 'configured' if OK)
 */

import {
  BleLinkState, LaneState, MsgId, NodeClass, NodeState,
} from '../protocol/generated.ts';
import type { NodeInfo } from '../protocol/generated.ts';
import type { DeviceEvent } from './deviceSession.ts';

export type LanePhase = 'absent' | 'joined' | 'configured' | 'fault';

export interface LaneNode {
  readonly index: number;
  readonly phase: LanePhase;
  readonly syncOk: boolean;
  /** ctrlStatus_e of the last FAULT, 0 if none. */
  readonly faultStatus: number;
  /** From NODE_LIST, when known. */
  readonly info: NodeInfo | null;
  /** sysUs of the last change, as a number (µs). */
  readonly changedUs: number;
}

export interface BleLink {
  readonly state: 'down' | 'up' | 'versionMismatch' | 'unknown';
  readonly phy: number;
  readonly attMtu: number;
  readonly intervalMs: number;
}

export interface NodeModel {
  readonly bootId: number | null;
  readonly ctrl: NodeInfo | null;
  readonly ble: NodeInfo | null;
  /** Lanes 1..6 (index 0 unused). */
  readonly lanes: readonly LaneNode[];
  readonly bleLink: BleLink;
  readonly seqRunning: boolean;
}

const emptyLane = (index: number): LaneNode => ({ index, phase: 'absent', syncOk: true, faultStatus: 0, info: null, changedUs: 0 });

export function emptyNodeModel(): NodeModel {
  return {
    bootId: null, ctrl: null, ble: null, lanes: Array.from({ length: 7 }, (_, i) => emptyLane(i)),
    bleLink: { state: 'unknown', phy: 0, attMtu: 0, intervalMs: 0 }, seqRunning: false,
  };
}

/** Lanes that are present (joined or configured or faulted), in index order. */
export function presentLanes(m: NodeModel): number[] {
  return m.lanes.filter((l) => l.index > 0 && l.phase !== 'absent').map((l) => l.index);
}

/** Lanes ready for timing: configured and SYNC ok. */
export function readyLanes(m: NodeModel): number[] {
  return m.lanes.filter((l) => l.index > 0 && l.phase === 'configured' && l.syncOk).map((l) => l.index);
}

export function applyNodeList(m: NodeModel, nodes: readonly NodeInfo[]): NodeModel {
  const lanes = m.lanes.map((l) => (l.index === 0 ? l : { ...l, info: null }));
  let ctrl: NodeInfo | null = null;
  let ble: NodeInfo | null = null;
  for (const n of nodes) {
    if (n.nodeClass === NodeClass.CTRL) ctrl = n;
    else if (n.nodeClass === NodeClass.BLE) ble = n;
    else if (n.nodeClass === NodeClass.LANE && n.index >= 1 && n.index <= 6) {
      const cur = lanes[n.index]!;
      // The firmware lists a lane from its join on: UPDATING while the config push runs,
      // OK once configured, FAULT when the push gave up or the protocol differs.
      const listed: LanePhase = n.state === NodeState.UPDATING ? 'joined' : 'configured';
      const phase: LanePhase = n.state === NodeState.FAULT ? 'fault' : cur.phase === 'absent' ? listed : cur.phase;
      lanes[n.index] = { ...cur, info: n, phase };
    }
  }
  for (const l of lanes) {
    if (l.index > 0 && l.phase !== 'absent' && !nodes.some((n) => n.nodeClass === NodeClass.LANE && n.index === l.index)) {
      lanes[l.index] = { ...l, phase: 'absent' };
    }
  }
  return { ...m, ctrl, ble, lanes };
}

export function applyNodeEvent(m: NodeModel, e: DeviceEvent): NodeModel {
  const v = e.value;
  if (!v) return m;
  const us = (x: unknown): number => (typeof x === 'bigint' ? Number(x) : 0);
  switch (e.msgId) {
    case MsgId.EVT_BOOT:
      return { ...emptyNodeModel(), bootId: e.bootId, ctrl: m.ctrl, ble: m.ble };
    case MsgId.EVT_LANE_STATE: {
      const idx = v['lane'] as number;
      if (idx < 1 || idx > 6) return m;
      const cur = m.lanes[idx]!;
      const t = us(v['sysUs']);
      let next: LaneNode = cur;
      switch (v['state']) {
        case LaneState.JOINED: next = { ...cur, phase: 'joined', syncOk: true, faultStatus: 0, changedUs: t }; break;
        case LaneState.CONFIGURED: next = { ...cur, phase: 'configured', changedUs: t }; break;
        case LaneState.LEFT: next = { ...cur, phase: 'absent', info: null, changedUs: t }; break;
        case LaneState.FAULT: next = { ...cur, phase: 'fault', faultStatus: v['status'] as number, changedUs: t }; break;
        case LaneState.SYNC_LOST: next = { ...cur, syncOk: false, changedUs: t }; break;
        case LaneState.SYNC_OK: next = { ...cur, syncOk: true, changedUs: t }; break;
        default: return m;
      }
      const lanes = m.lanes.slice();
      lanes[idx] = next;
      return { ...m, bootId: e.bootId, lanes };
    }
    case MsgId.EVT_BLE_LINK: {
      const st = v['state'] === BleLinkState.UP ? 'up' : v['state'] === BleLinkState.VERSION_MISMATCH ? 'versionMismatch' : 'down';
      return {
        ...m, bleLink: {
          state: st, phy: v['phy'] as number, attMtu: v['attMtu'] as number, intervalMs: (v['intervalUnits'] as number) * 1.25,
        },
      };
    }
    case MsgId.EVT_SEQ_STARTED: return { ...m, seqRunning: true };
    case MsgId.EVT_LIGHTS_OUT:
    case MsgId.EVT_SEQ_ABORTED: return { ...m, seqRunning: false };
    default: return m;
  }
}
