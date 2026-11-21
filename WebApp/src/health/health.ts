/*
 * Health rules: PURE evaluate(inputs, thresholds) → HealthReport.
 *
 * The status page of the web app shows one block per part: the control
 * module, the BLE chip (nRF52805 bridge) and lanes 1..6. Each part has an
 * online flag (the BLE chip also `connected`: a phone is connected) and a
 * list of checks. A check has a stable string id (CHECKS below: the GUI
 * lists them and may translate them by id), a level, a short message and the
 * measured value.
 *
 *   ok       the rule holds
 *   warn     works, but look at it (a counter moved, a margin is thin)
 *   fail     broken or about to break timing (offline, no SYNC, sensor gone,
 *            strip far too dirty, saturated)
 *   unknown  cannot be judged (part offline, no data yet, not applicable)
 *
 * overall = the worst level of every check (unknown ignored); 'fail' while
 * the control module is offline.
 *
 * Threshold edges (DEFAULT_THRESHOLDS, one object; deeper settings later):
 *   upper bounds (counters, loop time, noise ratio, saturation):
 *       value >= xWarn → warn, value >= xFail → fail
 *   lower bounds (signal level, drift ratio, RSSI):
 *       value <  xWarn → warn, value <  xFail → fail
 *
 * Inputs come from STATUS, the node model (NODE_LIST + events, incl.
 * EVT_LANE_CALIBRATED) and the last LANE_STATUS of each lane; HealthMonitor
 * (monitor.ts) collects them. Nothing here talks to the device.
 */

import { BleBridgeState, BleLinkState, ResetCause, statusName } from '../protocol/generated.ts';
import type { LaneStatusRsp, StatusRsp } from '../protocol/generated.ts';
import { LaneStatusFlag } from '../device/laneFlags.ts';
import type { NodeModel } from '../device/nodeModel.ts';

export type CheckLevel = 'ok' | 'warn' | 'fail' | 'unknown';
export type OverallLevel = 'ok' | 'warn' | 'fail';
export type CheckValue = number | string | boolean | null;

export interface Check {
  /** Stable id from CHECKS (e.g. 'lane.drift'). */
  readonly id: string;
  readonly level: CheckLevel;
  /** Short text for the status page. */
  readonly message: string;
  /** The measured value the level was judged on (null if none). */
  readonly value: CheckValue;
}

export interface PartHealth {
  readonly online: boolean;
  readonly checks: readonly Check[];
}

export interface BleHealth extends PartHealth {
  /** online = the nRF is alive (STATUS.bleBridge not ABSENT). connected = a phone (central) is connected (bleLink UP). */
  readonly connected: boolean;
}

export interface LaneHealth extends PartHealth {
  /** 1..6 */
  readonly index: number;
}

export interface HealthReport {
  readonly overall: OverallLevel;
  readonly control: PartHealth;
  readonly ble: BleHealth;
  /** Always six entries, lanes 1..6 in order. */
  readonly lanes: readonly LaneHealth[];
}

export interface HealthInputs {
  /** A session to the control module is open. */
  readonly connected: boolean;
  /** The last STATUS answer (null: none yet, or the last poll failed). */
  readonly status: StatusRsp | null;
  /** Age of `status` in ms. */
  readonly statusAgeMs: number | null;
  readonly nodes: NodeModel;
  /** Last LANE_STATUS per lane, index 1..6 (index 0 unused). */
  readonly laneStatus: readonly (LaneStatusRsp | null | undefined)[];
  /** Age of each laneStatus entry in ms. */
  readonly laneAgeMs?: readonly (number | null | undefined)[];
  /** Lanes that should be there (seen this session, or in the race): absent → fail instead of unknown. */
  readonly expectedLanes: readonly number[];
  /** The app's next expected evSeq of the current boot (event store position), if known. */
  readonly appNextEvSeq?: number | null;
}

export const DEFAULT_THRESHOLDS = {
  /** STATUS older than this: the link counts as not responding. */
  statusStaleMs: 6000,
  /** Super-loop pass (STATUS.loopMaxMs): the lane bus is polled every 10 ms. */
  loopWarnMs: 5,
  loopFailMs: 10,
  usbFaultsWarn: 1,
  usbFaultsFail: 3,
  /** STATUS.errorCount (faults since boot). */
  errorsWarn: 1,
  /** Events the app is behind the device log head. */
  eventLagWarn: 100,
  /** RSSI of the connected phone (dBm). */
  rssiWarnDbm: -80,
  rssiFailDbm: -90,
  /** bleCrcErrors + nrfLinkCrcErrors (STM ↔ nRF link frames). */
  linkCrcWarn: 1,
  linkCrcFail: 50,
  uartWarn: 1,
  uartFail: 50,
  bleDroppedWarn: 1,
  bleDroppedFail: 100,
  /** Tracked baseline (counts): too low = misaligned / dirty / IR too weak. */
  signalLowWarn: 1000,
  signalLowFail: 400,
  /** Baseline or rawMax at or above: the 16-bit sensor clips (lower the IR current). */
  saturationWarn: 60000,
  saturationFail: 65000,
  /** baseline / reference: below = dirty strip or moved module (clean, then recalibrate). */
  driftWarn: 0.85,
  driftFail: 0.70,
  /** baseline / reference above this: more light than at calibration (ambient light?): recalibrate. */
  driftHighWarn: 1.20,
  /** noise / (baseline - thrLow): the share of the detection margin the noise eats. */
  noiseWarn: 0.20,
  noiseFail: 0.33,
  droppedEventsWarn: 1,
  badWritesWarn: 1,
  lateActionsWarn: 1,
  /** A LANE_STATUS older than this is marked stale in the messages (during a race it is polled slowly). */
  laneStaleMs: 15000,
} as const;

export type HealthThresholds = { readonly [K in keyof typeof DEFAULT_THRESHOLDS]: number };

export type CheckPart = 'control' | 'ble' | 'lane';

export interface CheckInfo {
  readonly id: string;
  readonly part: CheckPart;
  readonly title: string;
  readonly description: string;
}

/** Every check, in report order per part. The GUI lists them from here. */
export const CHECKS: readonly CheckInfo[] = [
  { id: 'ctrl.link', part: 'control', title: 'Link responding', description: 'A session is open and STATUS answers (fail when not connected or STATUS is older than statusStaleMs).' },
  { id: 'ctrl.settings', part: 'control', title: 'Settings intact', description: 'STATUS.settingsDefaults: warn when the stored settings were lost at boot and defaults loaded (name, BLE, lane table, references).' },
  { id: 'ctrl.lanePower', part: 'control', title: 'Lane power', description: 'STATUS.lanesPowered: warn while the lane rail is off (no lane can join).' },
  { id: 'ctrl.usbFaults', part: 'control', title: 'USB faults', description: 'STATUS.usbFaults (USB-C port protector trips since boot): warn >= usbFaultsWarn, fail >= usbFaultsFail.' },
  { id: 'ctrl.errors', part: 'control', title: 'Errors', description: 'STATUS.errorCount / lastError (faults since boot): warn >= errorsWarn.' },
  { id: 'ctrl.loopTime', part: 'control', title: 'Loop time', description: 'STATUS.loopMaxMs, the longest super-loop pass: warn >= loopWarnMs, fail >= loopFailMs.' },
  { id: 'ctrl.eventLog', part: 'control', title: 'Event log', description: 'The app keeps up with the device event log: fail when events were overwritten before the app read them, warn when it lags >= eventLagWarn.' },
  { id: 'ble.bridge', part: 'ble', title: 'BLE chip present', description: 'STATUS.bleBridge: the nRF52805 heartbeats (ABSENT after 3 s of silence) → fail when absent.' },
  { id: 'ble.version', part: 'ble', title: 'BLE link version', description: 'STATUS.bleBridge VERSION_MISMATCH: the nRF link protocol differs, BLE data refused → fail.' },
  { id: 'ble.rssi', part: 'ble', title: 'Signal strength', description: 'STATUS.bleRssi of the connected phone: warn < rssiWarnDbm, fail < rssiFailDbm; unknown without a phone.' },
  { id: 'ble.linkCrc', part: 'ble', title: 'Link CRC errors', description: 'STATUS.bleCrcErrors + nrfLinkCrcErrors (bad frames between STM and nRF): warn >= linkCrcWarn, fail >= linkCrcFail.' },
  { id: 'ble.uart', part: 'ble', title: 'UART errors', description: 'STATUS.bleUartErrors (nRF UART framing/overrun): warn >= uartWarn, fail >= uartFail.' },
  { id: 'ble.dropped', part: 'ble', title: 'Dropped data', description: 'STATUS.bleDataDropped (data the nRF had to drop): warn >= bleDroppedWarn, fail >= bleDroppedFail.' },
  { id: 'lane.present', part: 'lane', title: 'Present', description: 'The lane is on the bus. Absent: fail if it was seen this session (or is in the race) and the lane rail is on, else unknown (not connected / lane power off).' },
  { id: 'lane.fault', part: 'lane', title: 'Fault', description: 'EVT_LANE_STATE FAULT → fail until the lane recovers (CONFIGURED); LANE_STATUS.lastError != 0 → warn.' },
  { id: 'lane.sensor', part: 'lane', title: 'Sensor', description: 'LANE_STATUS SENSOR_OK flag (VCNL4040 answers): fail when clear.' },
  { id: 'lane.sync', part: 'lane', title: 'SYNC', description: 'SYNC_LOCKED and SYNC_ALIGNED set, not SYNC_LOST: fail when lost or not locked, warn when not aligned.' },
  { id: 'lane.config', part: 'lane', title: 'Config', description: 'CONFIG_VALID flag: fail when clear; warn while the config push runs (joined).' },
  { id: 'lane.calibrated', part: 'lane', title: 'Baseline learned', description: 'CALIBRATED flag (baseline established) and a reference: warn while learning.' },
  { id: 'lane.savedReference', part: 'lane', title: 'Saved reference', description: 'A failed / not saved calibration this session wins (fail / warn). Else from LANE_STATUS: running on the saved calibration (reference == savedReference) ok; nothing saved, or the lane runs on another value, warn; no LANE_STATUS yet: the session outcome, else unknown.' },
  { id: 'lane.signal', part: 'lane', title: 'Signal level', description: 'Baseline: fail < signalLowFail, warn < signalLowWarn (misaligned / dirty); baseline or rawMax >= saturationFail fail, >= saturationWarn warn (lower the IR current).' },
  { id: 'lane.drift', part: 'lane', title: 'Drift', description: 'baseline / reference: warn < driftWarn, fail < driftFail (clean the strip, recalibrate); warn > driftHighWarn.' },
  { id: 'lane.noise', part: 'lane', title: 'Noise', description: 'noise / (baseline - thrLow): warn >= noiseWarn, fail >= noiseFail.' },
  { id: 'lane.blocked', part: 'lane', title: 'Strip free', description: 'BLOCKED flag (signal below LOW now): warn, something is on the strip.' },
  { id: 'lane.droppedEvents', part: 'lane', title: 'Dropped events', description: 'LANE_STATUS.droppedEvents or FIFO_OVERFLOW: warn >= droppedEventsWarn.' },
  { id: 'lane.badWrites', part: 'lane', title: 'Bad writes', description: 'LANE_STATUS.badWrites (rejected register writes): warn >= badWritesWarn.' },
  { id: 'lane.lateActions', part: 'lane', title: 'Late actions', description: 'LANE_STATUS.lateActions (scheduled light actions fired late): warn >= lateActionsWarn.' },
  { id: 'lane.reset', part: 'lane', title: 'Last reset', description: 'LANE_STATUS.resetCause WATCHDOG, BROWNOUT or FAULT: warn.' },
];

export const CHECK_IDS = {
  control: CHECKS.filter((c) => c.part === 'control').map((c) => c.id),
  ble: CHECKS.filter((c) => c.part === 'ble').map((c) => c.id),
  lane: CHECKS.filter((c) => c.part === 'lane').map((c) => c.id),
} as const;

const RANK: Record<CheckLevel, number> = { unknown: 0, ok: 1, warn: 2, fail: 3 };

/** The worst of the levels (unknown only when there is nothing else). */
export function worst(levels: Iterable<CheckLevel>): CheckLevel {
  let w: CheckLevel = 'unknown';
  for (const l of levels) if (RANK[l] > RANK[w]) w = l;
  return w;
}

const mk = (id: string, level: CheckLevel, message: string, value: CheckValue = null): Check => ({ id, level, message, value });

/** Upper bound: >= fail → fail, >= warn → warn. */
function upper(v: number, warn: number, fail: number): CheckLevel {
  return v >= fail ? 'fail' : v >= warn ? 'warn' : 'ok';
}

/** Lower bound: < fail → fail, < warn → warn. */
function lower(v: number, warn: number, fail: number): CheckLevel {
  return v < fail ? 'fail' : v < warn ? 'warn' : 'ok';
}

const round3 = (x: number): number => Math.round(x * 1000) / 1000;
const pct = (x: number): string => `${Math.round(x * 100)} %`;
const resetName = (c: number): string => Object.entries(ResetCause).find(([, v]) => v === c)?.[0] ?? String(c);

function controlChecks(i: HealthInputs, t: HealthThresholds, online: boolean): Check[] {
  const st = i.status;
  const out: Check[] = [];
  if (!i.connected) out.push(mk('ctrl.link', 'fail', 'not connected', false));
  else if (!st || i.statusAgeMs === null) out.push(mk('ctrl.link', 'fail', 'STATUS does not answer', null));
  else if (i.statusAgeMs > t.statusStaleMs) out.push(mk('ctrl.link', 'fail', `STATUS ${Math.round(i.statusAgeMs / 1000)} s old`, i.statusAgeMs));
  else out.push(mk('ctrl.link', 'ok', 'responding', i.statusAgeMs));
  if (!online || !st) {
    for (const id of CHECK_IDS.control.slice(1)) out.push(mk(id, 'unknown', 'control module offline'));
    return out;
  }
  out.push(st.settingsDefaults
    ? mk('ctrl.settings', 'warn', 'settings were lost at boot: defaults loaded (check name, BLE, lane table, references)', 1)
    : mk('ctrl.settings', 'ok', 'intact', 0));
  out.push(st.lanesPowered
    ? mk('ctrl.lanePower', 'ok', `on, ${st.lanesPresent} lane(s)`, st.lanesPresent)
    : mk('ctrl.lanePower', 'warn', 'lane power off', 0));
  const usb = upper(st.usbFaults, t.usbFaultsWarn, t.usbFaultsFail);
  out.push(mk('ctrl.usbFaults', usb, usb === 'ok' ? 'none' : `${st.usbFaults} USB fault trip(s) since boot`, st.usbFaults));
  const err = upper(st.errorCount, t.errorsWarn, Number.POSITIVE_INFINITY);
  out.push(mk('ctrl.errors', err, err === 'ok' ? 'none' : `${st.errorCount} error(s), last ${statusName(st.lastError)}`, st.errorCount));
  const loop = upper(st.loopMaxMs, t.loopWarnMs, t.loopFailMs);
  out.push(mk('ctrl.loopTime', loop, `longest loop ${st.loopMaxMs} ms`, st.loopMaxMs));
  const app = i.appNextEvSeq;
  if (app === null || app === undefined) {
    out.push(mk('ctrl.eventLog', 'ok', `log ${st.logOldestSeq}..${st.logNextSeq}`, st.logNextSeq - st.logOldestSeq));
  } else if (app < st.logOldestSeq) {
    out.push(mk('ctrl.eventLog', 'fail', `events ${app}..${st.logOldestSeq - 1} were overwritten before the app read them`, st.logOldestSeq - app));
  } else {
    const lag = Math.max(0, st.logNextSeq - app);
    const lvl = upper(lag, t.eventLagWarn, Number.POSITIVE_INFINITY);
    out.push(mk('ctrl.eventLog', lvl, lvl === 'ok' ? 'up to date' : `${lag} events behind`, lag));
  }
  return out;
}

function bleChecks(i: HealthInputs, t: HealthThresholds, ctrlOnline: boolean): { checks: Check[]; online: boolean; connected: boolean } {
  const st = i.status;
  if (!ctrlOnline || !st) {
    return { online: false, connected: false, checks: CHECK_IDS.ble.map((id) => mk(id, 'unknown', 'control module offline')) };
  }
  const online = st.bleBridge !== BleBridgeState.ABSENT;
  const connected = online && st.bleLink === BleLinkState.UP;
  const out: Check[] = [];
  if (st.bleBridge === BleBridgeState.ABSENT) out.push(mk('ble.bridge', 'fail', 'BLE chip silent or not fitted', 'absent'));
  else out.push(mk('ble.bridge', 'ok', 'alive', st.bleBridge === BleBridgeState.OK ? 'ok' : 'versionMismatch'));
  const fw = i.nodes.ble ? `${i.nodes.ble.fwMajor}.${i.nodes.ble.fwMinor}.${i.nodes.ble.fwPatch}` : null;
  if (st.bleBridge === BleBridgeState.VERSION_MISMATCH) out.push(mk('ble.version', 'fail', 'link protocol mismatch: BLE data refused (flash matching firmware)', fw));
  else if (st.bleBridge === BleBridgeState.ABSENT) out.push(mk('ble.version', 'unknown', 'BLE chip absent', null));
  else out.push(mk('ble.version', 'ok', fw ? `firmware ${fw}` : 'matches', fw));
  if (!connected) out.push(mk('ble.rssi', 'unknown', 'no phone connected', null));
  else if (st.bleRssi === 127) out.push(mk('ble.rssi', 'unknown', 'RSSI not reported', null));
  else out.push(mk('ble.rssi', lower(st.bleRssi, t.rssiWarnDbm, t.rssiFailDbm), `${st.bleRssi} dBm`, st.bleRssi));
  if (st.bleBridge === BleBridgeState.ABSENT) {
    out.push(mk('ble.linkCrc', 'unknown', 'BLE chip absent'), mk('ble.uart', 'unknown', 'BLE chip absent'), mk('ble.dropped', 'unknown', 'BLE chip absent'));
    return { online, connected, checks: out };
  }
  const crc = st.bleCrcErrors + st.nrfLinkCrcErrors;
  out.push(mk('ble.linkCrc', upper(crc, t.linkCrcWarn, t.linkCrcFail), crc ? `${st.bleCrcErrors} at the nRF, ${st.nrfLinkCrcErrors} at the STM` : 'none', crc));
  out.push(mk('ble.uart', upper(st.bleUartErrors, t.uartWarn, t.uartFail), st.bleUartErrors ? `${st.bleUartErrors} UART error(s)` : 'none', st.bleUartErrors));
  out.push(mk('ble.dropped', upper(st.bleDataDropped, t.bleDroppedWarn, t.bleDroppedFail), st.bleDataDropped ? `${st.bleDataDropped} dropped` : 'none', st.bleDataDropped));
  return { online, connected, checks: out };
}

function laneChecks(i: HealthInputs, t: HealthThresholds, lane: number, ctrlOnline: boolean): LaneHealth {
  const node = i.nodes.lanes[lane];
  const online = ctrlOnline && !!node && node.phase !== 'absent';
  const rest = (msg: string): Check[] => CHECK_IDS.lane.slice(1).map((id) => mk(id, 'unknown', msg));
  if (!ctrlOnline) return { index: lane, online, checks: [mk('lane.present', 'unknown', 'control module offline'), ...rest('control module offline')] };
  if (!online) {
    const railOff = i.status?.lanesPowered === 0;
    const expected = i.expectedLanes.includes(lane) && !railOff;
    return {
      index: lane, online, checks: [
        mk('lane.present', expected ? 'fail' : 'unknown', expected ? 'offline' : railOff ? 'lane power off' : 'not connected', false),
        ...rest('lane offline'),
      ],
    };
  }
  const out: Check[] = [mk('lane.present', 'ok', node.phase, true)];
  const ls = i.laneStatus[lane] ?? null;
  const age = i.laneAgeMs?.[lane];
  const stale = ls && age !== null && age !== undefined && age > t.laneStaleMs ? ` (${Math.round(age / 1000)} s old)` : '';

  if (node.phase === 'fault') out.push(mk('lane.fault', 'fail', `fault ${statusName(node.faultStatus)}`, node.faultStatus));
  else if (ls && ls.lastError !== 0) out.push(mk('lane.fault', 'warn', `last error ${statusName(ls.lastError)}${stale}`, ls.lastError));
  else out.push(mk('lane.fault', 'ok', 'none', 0));

  if (!ls) {
    for (const id of CHECK_IDS.lane.slice(2)) {
      out.push(id === 'lane.savedReference' ? savedReference(node.calibration, null, null) : mk(id, 'unknown', 'no LANE_STATUS yet'));
    }
    return { index: lane, online, checks: out };
  }
  const f = ls.flags;
  const has = (bit: number): boolean => (f & bit) !== 0;

  out.push(has(LaneStatusFlag.SENSOR_OK) ? mk('lane.sensor', 'ok', `ok${stale}`, true) : mk('lane.sensor', 'fail', `sensor not answering${stale}`, false));

  if (has(LaneStatusFlag.SYNC_LOST) || !node.syncOk) out.push(mk('lane.sync', 'fail', `SYNC lost${stale}`, ls.syncShifts));
  else if (!has(LaneStatusFlag.SYNC_LOCKED)) out.push(mk('lane.sync', 'fail', `no SYNC lock${stale}`, ls.syncShifts));
  else if (!has(LaneStatusFlag.SYNC_ALIGNED)) out.push(mk('lane.sync', 'warn', `not aligned yet${stale}`, ls.syncShifts));
  else out.push(mk('lane.sync', 'ok', `locked${ls.syncShifts ? `, ${ls.syncShifts} shift(s)` : ''}${stale}`, ls.syncShifts));

  if (!has(LaneStatusFlag.CONFIG_VALID)) out.push(mk('lane.config', node.phase === 'joined' ? 'warn' : 'fail', node.phase === 'joined' ? 'config push running' : `no valid config${stale}`, false));
  else out.push(mk('lane.config', 'ok', 'valid', true));

  const learned = has(LaneStatusFlag.CALIBRATED) && ls.baseline > 0;
  out.push(learned
    ? mk('lane.calibrated', 'ok', `baseline ${ls.baseline}, reference ${ls.reference}${stale}`, ls.reference)
    : mk('lane.calibrated', 'warn', `learning the baseline${stale}`, ls.reference));

  out.push(savedReference(node.calibration, ls.reference, ls.savedReference));

  const peak = Math.max(ls.baseline, ls.rawMax);
  if (!learned && peak < t.saturationWarn) out.push(mk('lane.signal', 'unknown', 'baseline not learned yet', ls.raw));
  else if (peak >= t.saturationFail) out.push(mk('lane.signal', 'fail', `saturated (${peak}): lower the IR current`, peak));
  else if (peak >= t.saturationWarn) out.push(mk('lane.signal', 'warn', `near saturation (${peak}): lower the IR current`, peak));
  else {
    const lvl = lower(ls.baseline, t.signalLowWarn, t.signalLowFail);
    out.push(mk('lane.signal', lvl, lvl === 'ok' ? `level ${ls.baseline}${stale}` : `level ${ls.baseline} too low: misaligned or dirty strip${stale}`, ls.baseline));
  }

  if (!learned || ls.reference === 0) out.push(mk('lane.drift', 'unknown', 'no reference yet', null));
  else {
    const r = ls.baseline / ls.reference;
    let lvl = lower(r, t.driftWarn, t.driftFail);
    let msg = `${pct(r)} of reference${stale}`;
    if (lvl !== 'ok') msg = `${pct(r)} of reference: clean the strip, then recalibrate${stale}`;
    else if (r > t.driftHighWarn) { lvl = 'warn'; msg = `${pct(r)} of reference (more light than at calibration): recalibrate${stale}`; }
    out.push(mk('lane.drift', lvl, msg, round3(r)));
  }

  const margin = ls.baseline - ls.thrLow;
  if (!learned || margin <= 0) out.push(mk('lane.noise', 'unknown', 'no detection margin yet', ls.noise));
  else {
    const r = ls.noise / margin;
    const lvl = upper(r, t.noiseWarn, t.noiseFail);
    out.push(mk('lane.noise', lvl, `noise ${ls.noise} = ${pct(r)} of the margin${stale}`, round3(r)));
  }

  out.push(has(LaneStatusFlag.BLOCKED) ? mk('lane.blocked', 'warn', `something is on the strip${stale}`, true) : mk('lane.blocked', 'ok', 'free', false));

  const dropLvl = has(LaneStatusFlag.FIFO_OVERFLOW) ? worst(['warn', upper(ls.droppedEvents, t.droppedEventsWarn, Number.POSITIVE_INFINITY)])
    : upper(ls.droppedEvents, t.droppedEventsWarn, Number.POSITIVE_INFINITY);
  out.push(mk('lane.droppedEvents', dropLvl, dropLvl === 'ok' ? 'none' : `${ls.droppedEvents} event(s) dropped on the lane`, ls.droppedEvents));
  const bw = upper(ls.badWrites, t.badWritesWarn, Number.POSITIVE_INFINITY);
  out.push(mk('lane.badWrites', bw, bw === 'ok' ? 'none' : `${ls.badWrites} rejected write(s)`, ls.badWrites));
  const la = upper(ls.lateActions, t.lateActionsWarn, Number.POSITIVE_INFINITY);
  out.push(mk('lane.lateActions', la, la === 'ok' ? 'none' : `${ls.lateActions} late action(s)`, ls.lateActions));
  const bad = ls.resetCause === ResetCause.WATCHDOG || ls.resetCause === ResetCause.BROWNOUT || ls.resetCause === ResetCause.FAULT;
  out.push(mk('lane.reset', bad ? 'warn' : 'ok', `last reset ${resetName(ls.resetCause)}`, resetName(ls.resetCause)));
  return { index: lane, online, checks: out };
}

function savedReference(cal: NodeModel['lanes'][number]['calibration'], reference: number | null, saved: number | null): Check {
  const id = 'lane.savedReference';
  // The last calibration of this session failing (or not reaching flash) is the news.
  if (cal?.result === 'failed') return mk(id, 'fail', cal.timedOut ? 'calibration timed out' : 'calibration failed (lane busy or left)', 0);
  if (cal?.result === 'notSaved') return mk(id, 'warn', `learned ${cal.reference} but not saved yet (the control module retries)`, cal.reference);
  // LANE_STATUS.savedReference: what the control module keeps for this lane index.
  if (reference !== null && saved !== null) {
    if (saved === 0) return mk(id, 'warn', `nothing saved: the lane learned ${reference} at power-up; calibrate with the lane free`, 0);
    if (reference === saved) return mk(id, 'ok', `running on the saved calibration ${saved}`, saved);
    return mk(id, 'warn', `lane runs on ${reference}, saved is ${saved}: recalibrate`, saved);
  }
  if (!cal) return mk(id, 'unknown', 'no LANE_STATUS yet', null);
  // failed / notSaved returned above: only saved and forgotten are left.
  return cal.result === 'saved' ? mk(id, 'ok', `saved ${cal.reference}`, cal.reference)
    : mk(id, 'warn', 'no saved reference: the lane learns at every power-up', 0);
}

/** The health report. Pure: same inputs, same report. */
export function evaluate(inputs: HealthInputs, thresholds: HealthThresholds = DEFAULT_THRESHOLDS): HealthReport {
  const t = thresholds;
  const ctrlOnline = inputs.connected && inputs.status !== null && inputs.statusAgeMs !== null && inputs.statusAgeMs <= t.statusStaleMs;
  const control: PartHealth = { online: ctrlOnline, checks: controlChecks(inputs, t, ctrlOnline) };
  const b = bleChecks(inputs, t, ctrlOnline);
  const ble: BleHealth = { online: b.online, connected: b.connected, checks: b.checks };
  const lanes: LaneHealth[] = [];
  for (let l = 1; l <= 6; l++) lanes.push(laneChecks(inputs, t, l, ctrlOnline));
  const all = [...control.checks, ...ble.checks, ...lanes.flatMap((l) => l.checks)].map((c) => c.level);
  const w = worst(all);
  // Offline control module: ctrl.link is 'fail', so overall is 'fail' too.
  const overall: OverallLevel = w === 'unknown' ? 'ok' : w;
  return { overall, control, ble, lanes };
}

/** One check of a report by part and id (tests, GUI). */
export function findCheck(r: HealthReport, id: string, lane?: number): Check | undefined {
  const list = id.startsWith('ctrl.') ? r.control.checks : id.startsWith('ble.') ? r.ble.checks : r.lanes[(lane ?? 1) - 1]?.checks ?? [];
  return list.find((c) => c.id === id);
}
