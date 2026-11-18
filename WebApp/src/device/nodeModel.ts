/*
 * The node model: what is attached to the control module right now, built
 * from NODE_LIST answers plus the events that change it (SYSTEM_ARCHITECTURE
 * §7.5: "the web app builds its UI from it").
 *
 * Pure and immutable: applyNodeList / applyNodeEvent return a new model.
 *
 *   EVT_BOOT        the device rebooted: lanes gone (the rail is off after
 *                   every boot), seq not running, BLE link unknown,
 *                   calibration outcomes of the old boot dropped
 *   EVT_LANE_STATE  JOINED / CONFIGURED / LEFT / FAULT / SYNC_LOST / SYNC_OK
 *                   (0.6: CONFIGURED after FAULT = the lane recovered; it
 *                   clears the fault)
 *   EVT_BLE_LINK    the BLE link to a central (a phone connected or not)
 *   EVT_LANE_CALIBRATED  the lane's last LANE_CALIBRATE outcome
 *   EVT_SEQ_*       whether a start sequence runs
 *   NODE_LIST       versions and board types (lanes listed are present; a
 *                   lane the model had as absent becomes 'joined' if listed
 *                   UPDATING (config push running), 'configured' if OK; a
 *                   faulted lane listed OK is configured again)
 *   STATUS          (applyStatus) the nRF bridge itself: bleBridge
 *                   absent / ok / versionMismatch, and seqRunning
 */

import {
  BleBridgeState, BleLinkState, CalibrateResult, LaneState, MsgId, NodeClass, NodeState,
} from '../protocol/generated.ts';
import type { NodeInfo, StatusRsp } from '../protocol/generated.ts';
import type { DeviceEvent } from './deviceSession.ts';

export type LanePhase = 'absent' | 'joined' | 'configured' | 'fault';

export type CalibrationResult = 'saved' | 'forgotten' | 'failed' | 'notSaved';

/** One EVT_LANE_CALIBRATED (or, from HealthMonitor, a timeout: result 'failed', timedOut). */
export interface LaneCalibration {
  readonly result: CalibrationResult;
  /** The new reference (saved / notSaved), else 0. */
  readonly reference: number;
  readonly noise: number;
  /** Device time (µs) of the event; null when synthesized by the host (timeout). */
  readonly sysUs: number | null;
  readonly bootId: number | null;
  readonly timedOut?: boolean;
}

export function calibrationResultOf(code: number): CalibrationResult {
  switch (code) {
    case CalibrateResult.SAVED: return 'saved';
    case CalibrateResult.FORGOTTEN: return 'forgotten';
    case CalibrateResult.NOT_SAVED: return 'notSaved';
    default: return 'failed';
  }
}

export type BleBridge = 'unknown' | 'absent' | 'ok' | 'versionMismatch';

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
  /** The last LANE_CALIBRATE outcome of this boot, null if none. */
  readonly calibration: LaneCalibration | null;
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
  /** The nRF52805 itself (STATUS.bleBridge, 0.6+): alive or not, independent of a phone. */
  readonly bleBridge: BleBridge;
  readonly seqRunning: boolean;
}

const emptyLane = (index: number): LaneNode => ({
  index, phase: 'absent', syncOk: true, faultStatus: 0, info: null, changedUs: 0, calibration: null,
});

export function emptyNodeModel(): NodeModel {
  return {
    bootId: null, ctrl: null, ble: null, lanes: Array.from({ length: 7 }, (_, i) => emptyLane(i)),
    bleLink: { state: 'unknown', phy: 0, attMtu: 0, intervalMs: 0 }, bleBridge: 'unknown', seqRunning: false,
  };
}

/** STATUS: the BLE bridge state and whether a start sequence runs. */
export function applyStatus(m: NodeModel, st: Pick<StatusRsp, 'bleBridge' | 'seqRunning'>): NodeModel {
  const bleBridge: BleBridge = st.bleBridge === BleBridgeState.OK ? 'ok'
    : st.bleBridge === BleBridgeState.VERSION_MISMATCH ? 'versionMismatch' : 'absent';
  const seqRunning = st.seqRunning === 1;
  if (bleBridge === m.bleBridge && seqRunning === m.seqRunning) return m;
  return { ...m, bleBridge, seqRunning };
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
      const phase: LanePhase = n.state === NodeState.FAULT ? 'fault'
        : cur.phase === 'absent' ? listed : cur.phase === 'fault' && n.state === NodeState.OK ? 'configured' : cur.phase;
      lanes[n.index] = { ...cur, info: n, phase, faultStatus: phase === 'fault' ? cur.faultStatus : 0 };
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
      return { ...emptyNodeModel(), bootId: e.bootId, ctrl: m.ctrl, ble: m.ble, bleBridge: m.bleBridge };
    case MsgId.EVT_LANE_STATE: {
      const idx = v['lane'] as number;
      if (idx < 1 || idx > 6) return m;
      const cur = m.lanes[idx]!;
      const t = us(v['sysUs']);
      let next: LaneNode = cur;
      switch (v['state']) {
        case LaneState.JOINED: next = { ...cur, phase: 'joined', syncOk: true, faultStatus: 0, changedUs: t }; break;
        // 0.6: also the recovery of a faulted lane.
        case LaneState.CONFIGURED: next = { ...cur, phase: 'configured', faultStatus: 0, changedUs: t }; break;
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
    case MsgId.EVT_LANE_CALIBRATED: {
      const idx = v['lane'] as number;
      if (idx < 1 || idx > 6) return m;
      const lanes = m.lanes.slice();
      lanes[idx] = {
        ...m.lanes[idx]!, calibration: {
          result: calibrationResultOf(v['result'] as number), reference: v['reference'] as number,
          noise: v['noise'] as number, sysUs: us(v['sysUs']), bootId: e.bootId,
        },
      };
      return { ...m, lanes };
    }
    case MsgId.EVT_SEQ_STARTED: return { ...m, seqRunning: true };
    case MsgId.EVT_LIGHTS_OUT:
    case MsgId.EVT_SEQ_ABORTED: return { ...m, seqRunning: false };
    default: return m;
  }
}
