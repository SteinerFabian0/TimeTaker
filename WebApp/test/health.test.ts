/*
 * Health rules (src/health/health.ts): a healthy baseline, then one broken
 * rule per case, with every threshold edge on both sides.
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { LaneStatusFlag } from '../src/device/laneFlags.ts';
import { applyNodeEvent, applyNodeList, applyStatus, emptyNodeModel } from '../src/device/nodeModel.ts';
import type { LaneCalibration, NodeModel } from '../src/device/nodeModel.ts';
import type { DeviceEvent } from '../src/device/deviceSession.ts';
import { CHECK_IDS, CHECKS, DEFAULT_THRESHOLDS, evaluate, findCheck, worst } from '../src/health/health.ts';
import type { CheckLevel, HealthInputs } from '../src/health/health.ts';
import {
  BleBridgeState, BleLinkState, BoardType, CalibrateResult, LaneBusState, LaneState, MsgId, NodeClass, NodeState, ResetCause,
  SamplingMode, Status,
} from '../src/protocol/generated.ts';
import type { LaneStatusRsp, NodeInfo, StatusRsp } from '../src/protocol/generated.ts';

const GOOD_FLAGS = LaneStatusFlag.SYNC_LOCKED | LaneStatusFlag.SYNC_ALIGNED | LaneStatusFlag.SENSOR_OK
  | LaneStatusFlag.CONFIG_VALID | LaneStatusFlag.CALIBRATED;

function status(p: Partial<StatusRsp> = {}): StatusRsp {
  return {
    status: 0, lastError: 0, loopMaxMs: 2, errorCount: 0, uptimeMs: 10000, lanesPresent: 2, lanesPowered: 1,
    bleLink: BleLinkState.UP, seqRunning: 0, usbFaults: 0, settingsWrites: 0, logOldestSeq: 0, logNextSeq: 50,
    bleBridge: BleBridgeState.OK, bleRssi: -60, bleCrcErrors: 0, bleUartErrors: 0, bleDataDropped: 0, nrfLinkCrcErrors: 0,
    settingsDefaults: 0, reserved: 0, ...p,
  };
}

function laneStatus(lane: number, p: Partial<LaneStatusRsp> = {}): LaneStatusRsp {
  return {
    status: 0, lane, state: LaneBusState.SAMPLING, samplingMode: SamplingMode.ON, fifoCount: 0, flags: GOOD_FLAGS,
    droppedEvents: 0, syncShifts: 0, badWrites: 0, lateActions: 0, lastError: 0, resetCause: ResetCause.POWER_ON, faultId: 0,
    uptimeMs: 5000, raw: 3100, baseline: 3100, thrLow: 2100, thrHigh: 2400, noise: 10, reference: 3100, rawMin: 3080, rawMax: 3120,
    savedReference: 3100, ...p,
  };
}

const node = (nodeClass: number, index: number, state: number = NodeState.OK): NodeInfo => ({
  nodeClass, index, boardType: nodeClass === NodeClass.LANE ? BoardType.LANE : nodeClass === NodeClass.BLE ? BoardType.BLE_BRIDGE : BoardType.CONTROL,
  state, fwMajor: 0, fwMinor: 1, fwPatch: 0, protoMinor: 6, fwGitRev: 0, imageCrc: 0,
});

const ev = (msgId: number, value: Record<string, unknown>): DeviceEvent =>
  ({ bootId: 1, evSeq: 1, msgId, name: '', value } as unknown as DeviceEvent);

function nodes(): NodeModel {
  let m = applyNodeList(emptyNodeModel(), [node(NodeClass.CTRL, 0), node(NodeClass.BLE, 0), node(NodeClass.LANE, 1), node(NodeClass.LANE, 2)]);
  m = applyNodeEvent(m, ev(MsgId.EVT_LANE_CALIBRATED, { lane: 1, result: CalibrateResult.SAVED, reference: 3100, noise: 10, sysUs: 5n }));
  return m;
}

function inputs(p: Partial<HealthInputs> = {}): HealthInputs {
  return {
    connected: true, status: status(), statusAgeMs: 100, nodes: nodes(),
    laneStatus: [null, laneStatus(1), laneStatus(2)], laneAgeMs: [null, 50, 50], expectedLanes: [1, 2], appNextEvSeq: 50, ...p,
  };
}

const withLane = (lane: number, p: Partial<LaneStatusRsp>): Partial<HealthInputs> => {
  const ls: (LaneStatusRsp | null)[] = [null, laneStatus(1), laneStatus(2)];
  ls[lane] = laneStatus(lane, p);
  return { laneStatus: ls };
};

const withCal = (lane: number, cal: LaneCalibration | null): Partial<HealthInputs> => {
  const m = nodes();
  const lanes = m.lanes.slice();
  lanes[lane] = { ...lanes[lane]!, calibration: cal };
  return { nodes: { ...m, lanes } };
};

function level(p: Partial<HealthInputs>, id: string, lane = 1): CheckLevel {
  const c = findCheck(evaluate(inputs(p)), id, lane);
  assert.ok(c, `no check ${id}`);
  return c.level;
}

describe('health: healthy baseline and catalogue', () => {
  test('everything ok; lanes 3..6 not connected are unknown, not failures', () => {
    const r = evaluate(inputs());
    assert.equal(r.overall, 'ok');
    assert.equal(r.control.online, true);
    assert.deepEqual([r.ble.online, r.ble.connected], [true, true]);
    assert.deepEqual(r.lanes.map((l) => l.online), [true, true, false, false, false, false]);
    for (const c of [...r.control.checks, ...r.ble.checks, ...r.lanes[0]!.checks]) assert.equal(c.level, 'ok', `${c.id}: ${c.message}`);
    assert.match(findCheck(r, 'lane.savedReference', 2)!.message, /saved calibration 3100/);   // from LANE_STATUS, no outcome this session
    for (const c of r.lanes[3]!.checks) assert.equal(c.level, 'unknown');
  });

  test('catalogue: unique ids, every report lists exactly the catalogue in order', () => {
    const ids = CHECKS.map((c) => c.id);
    assert.equal(new Set(ids).size, ids.length);
    for (const c of CHECKS) assert.ok(c.title && c.description.length > 10, c.id);
    for (const p of [inputs(), inputs({ connected: false }), inputs({ laneStatus: [] })]) {
      const r = evaluate(p);
      assert.deepEqual(r.control.checks.map((c) => c.id), CHECK_IDS.control);
      assert.deepEqual(r.ble.checks.map((c) => c.id), CHECK_IDS.ble);
      for (const l of r.lanes) assert.deepEqual(l.checks.map((c) => c.id), CHECK_IDS.lane);
      assert.equal(r.lanes.length, 6);
    }
  });

  test('worst(): fail > warn > ok > unknown; overall ignores unknown', () => {
    assert.equal(worst(['ok', 'unknown', 'warn']), 'warn');
    assert.equal(worst(['ok', 'fail', 'warn']), 'fail');
    assert.equal(worst(['unknown']), 'unknown');
    assert.equal(worst([]), 'unknown');
    assert.equal(evaluate(inputs(withLane(1, { badWrites: 1 }))).overall, 'warn');
    assert.equal(evaluate(inputs(withLane(1, { flags: GOOD_FLAGS & ~LaneStatusFlag.SENSOR_OK }))).overall, 'fail');
  });

  test('pure: same inputs, same report', () => {
    assert.deepEqual(evaluate(inputs()), evaluate(inputs()));
  });
});

describe('health: control module', () => {
  test('link: not connected → fail and everything else unknown, overall fail', () => {
    const r = evaluate(inputs({ connected: false }));
    assert.equal(r.overall, 'fail');
    assert.equal(r.control.online, false);
    assert.equal(findCheck(r, 'ctrl.link')!.level, 'fail');
    assert.ok([...r.control.checks.slice(1), ...r.ble.checks, ...r.lanes.flatMap((l) => l.checks)].every((c) => c.level === 'unknown'));
    assert.deepEqual(r.lanes.map((l) => l.online), [false, false, false, false, false, false]);
  });
  test('link: no STATUS answer → fail; age edge at statusStaleMs', () => {
    assert.equal(level({ status: null, statusAgeMs: null }, 'ctrl.link'), 'fail');
    assert.equal(level({ statusAgeMs: DEFAULT_THRESHOLDS.statusStaleMs }, 'ctrl.link'), 'ok');
    assert.equal(level({ statusAgeMs: DEFAULT_THRESHOLDS.statusStaleMs + 1 }, 'ctrl.link'), 'fail');
    assert.equal(evaluate(inputs({ statusAgeMs: DEFAULT_THRESHOLDS.statusStaleMs + 1 })).control.online, false);
  });
  test('settings lost at boot → warn', () => {
    assert.equal(level({ status: status({ settingsDefaults: 1 }) }, 'ctrl.settings'), 'warn');
  });
  test('lane power off → warn; absent lanes are then unknown, not failed', () => {
    const r = evaluate(inputs({ status: status({ lanesPowered: 0, lanesPresent: 0 }), nodes: emptyNodeModel(), laneStatus: [] }));
    assert.equal(findCheck(r, 'ctrl.lanePower')!.level, 'warn');
    assert.equal(findCheck(r, 'lane.present', 1)!.level, 'unknown');
    assert.match(findCheck(r, 'lane.present', 1)!.message, /power off/);
  });
  test('USB faults: 0 ok, 1 warn, 2 warn, 3 fail', () => {
    assert.deepEqual([0, 1, 2, 3].map((n) => level({ status: status({ usbFaults: n }) }, 'ctrl.usbFaults')), ['ok', 'warn', 'warn', 'fail']);
  });
  test('errors: 0 ok, 1 warn with the last error named', () => {
    assert.equal(level({ status: status({ errorCount: 0 }) }, 'ctrl.errors'), 'ok');
    const c = findCheck(evaluate(inputs({ status: status({ errorCount: 1, lastError: Status.ERR_LANE_NACK }) })), 'ctrl.errors')!;
    assert.equal(c.level, 'warn');
    assert.match(c.message, /ERR_LANE_NACK/);
  });
  test('loop time: 4 ok, 5 warn, 9 warn, 10 fail', () => {
    assert.deepEqual([4, 5, 9, 10].map((n) => level({ status: status({ loopMaxMs: n }) }, 'ctrl.loopTime')), ['ok', 'warn', 'warn', 'fail']);
  });
  test('event log: overwritten unread → fail; lag 99 ok, 100 warn; unknown position ok', () => {
    assert.equal(level({ status: status({ logOldestSeq: 10, logNextSeq: 1610 }), appNextEvSeq: 9 }, 'ctrl.eventLog'), 'fail');
    assert.equal(level({ status: status({ logNextSeq: 149 }), appNextEvSeq: 50 }, 'ctrl.eventLog'), 'ok');
    assert.equal(level({ status: status({ logNextSeq: 150 }), appNextEvSeq: 50 }, 'ctrl.eventLog'), 'warn');
    assert.equal(level({ appNextEvSeq: null }, 'ctrl.eventLog'), 'ok');
  });
});

describe('health: BLE chip', () => {
  test('bridge ABSENT → fail, offline and not connected; counters unknown', () => {
    const r = evaluate(inputs({ status: status({ bleBridge: BleBridgeState.ABSENT, bleLink: BleLinkState.DOWN }) }));
    assert.deepEqual([r.ble.online, r.ble.connected], [false, false]);
    assert.equal(findCheck(r, 'ble.bridge')!.level, 'fail');
    assert.equal(findCheck(r, 'ble.version')!.level, 'unknown');
    assert.equal(findCheck(r, 'ble.linkCrc')!.level, 'unknown');
  });
  test('VERSION_MISMATCH → version fail; the chip itself is alive', () => {
    const r = evaluate(inputs({ status: status({ bleBridge: BleBridgeState.VERSION_MISMATCH, bleLink: BleLinkState.DOWN }) }));
    assert.equal(findCheck(r, 'ble.version')!.level, 'fail');
    assert.equal(findCheck(r, 'ble.bridge')!.level, 'ok');
    assert.deepEqual([r.ble.online, r.ble.connected], [true, false]);
  });
  test('no phone: online, not connected, RSSI unknown', () => {
    const r = evaluate(inputs({ status: status({ bleLink: BleLinkState.DOWN, bleRssi: 127 }) }));
    assert.deepEqual([r.ble.online, r.ble.connected], [true, false]);
    assert.equal(findCheck(r, 'ble.rssi')!.level, 'unknown');
  });
  test('RSSI: -80 ok, -81 warn, -90 warn, -91 fail, 127 unknown', () => {
    assert.deepEqual([-80, -81, -90, -91, 127].map((n) => level({ status: status({ bleRssi: n }) }, 'ble.rssi')), ['ok', 'warn', 'warn', 'fail', 'unknown']);
  });
  test('link CRC (both ends summed): 0 ok, 1 warn, 49 warn, 50 fail', () => {
    assert.equal(level({ status: status({ nrfLinkCrcErrors: 1 }) }, 'ble.linkCrc'), 'warn');
    assert.equal(level({ status: status({ bleCrcErrors: 24, nrfLinkCrcErrors: 25 }) }, 'ble.linkCrc'), 'warn');
    assert.equal(level({ status: status({ bleCrcErrors: 25, nrfLinkCrcErrors: 25 }) }, 'ble.linkCrc'), 'fail');
  });
  test('UART: 1 warn, 49 warn, 50 fail; dropped: 1 warn, 99 warn, 100 fail', () => {
    assert.deepEqual([0, 1, 49, 50].map((n) => level({ status: status({ bleUartErrors: n }) }, 'ble.uart')), ['ok', 'warn', 'warn', 'fail']);
    assert.deepEqual([0, 1, 99, 100].map((n) => level({ status: status({ bleDataDropped: n }) }, 'ble.dropped')), ['ok', 'warn', 'warn', 'fail']);
  });
});

describe('health: lanes', () => {
  test('present: absent and expected → fail; absent, not expected → unknown; other checks unknown', () => {
    const m = applyNodeEvent(nodes(), ev(MsgId.EVT_LANE_STATE, { lane: 2, state: LaneState.LEFT, status: 0, sysUs: 9n }));
    const r = evaluate(inputs({ nodes: m }));
    assert.equal(r.lanes[1]!.online, false);
    assert.equal(findCheck(r, 'lane.present', 2)!.level, 'fail');
    assert.ok(r.lanes[1]!.checks.slice(1).every((c) => c.level === 'unknown'));
    assert.equal(findCheck(evaluate(inputs({ nodes: m, expectedLanes: [1] })), 'lane.present', 2)!.level, 'unknown');
  });
  test('fault: FAULT event → fail; recovery (CONFIGURED) → ok; lastError → warn', () => {
    const f = applyNodeEvent(nodes(), ev(MsgId.EVT_LANE_STATE, { lane: 1, state: LaneState.FAULT, status: Status.ERR_HAL, sysUs: 9n }));
    const c = findCheck(evaluate(inputs({ nodes: f })), 'lane.fault', 1)!;
    assert.equal(c.level, 'fail');
    assert.match(c.message, /ERR_HAL/);
    const ok = applyNodeEvent(f, ev(MsgId.EVT_LANE_STATE, { lane: 1, state: LaneState.CONFIGURED, status: 0, sysUs: 10n }));
    assert.equal(findCheck(evaluate(inputs({ nodes: ok })), 'lane.fault', 1)!.level, 'ok');
    assert.equal(level(withLane(1, { lastError: Status.ERR_I3C }), 'lane.fault'), 'warn');
  });
  test('sensor: SENSOR_OK clear → fail', () => {
    assert.equal(level(withLane(1, { flags: GOOD_FLAGS & ~LaneStatusFlag.SENSOR_OK }), 'lane.sensor'), 'fail');
  });
  test('sync: lost → fail, not locked → fail, not aligned → warn, node SYNC_LOST → fail', () => {
    assert.equal(level(withLane(1, { flags: GOOD_FLAGS | LaneStatusFlag.SYNC_LOST }), 'lane.sync'), 'fail');
    assert.equal(level(withLane(1, { flags: GOOD_FLAGS & ~LaneStatusFlag.SYNC_LOCKED }), 'lane.sync'), 'fail');
    assert.equal(level(withLane(1, { flags: GOOD_FLAGS & ~LaneStatusFlag.SYNC_ALIGNED }), 'lane.sync'), 'warn');
    const m = applyNodeEvent(nodes(), ev(MsgId.EVT_LANE_STATE, { lane: 1, state: LaneState.SYNC_LOST, status: 0, sysUs: 9n }));
    assert.equal(level({ nodes: m }, 'lane.sync'), 'fail');
  });
  test('config: CONFIG_VALID clear → fail (warn while joined)', () => {
    assert.equal(level(withLane(1, { flags: GOOD_FLAGS & ~LaneStatusFlag.CONFIG_VALID }), 'lane.config'), 'fail');
    const m = applyNodeList(emptyNodeModel(), [node(NodeClass.LANE, 1, NodeState.UPDATING), node(NodeClass.LANE, 2)]);
    assert.equal(level({ nodes: m, ...withLane(1, { flags: GOOD_FLAGS & ~LaneStatusFlag.CONFIG_VALID }) }, 'lane.config'), 'warn');
  });
  test('calibrated: CALIBRATED clear → warn; signal, drift and noise cannot be judged then', () => {
    const p = withLane(1, { flags: GOOD_FLAGS & ~LaneStatusFlag.CALIBRATED });
    assert.equal(level(p, 'lane.calibrated'), 'warn');
    assert.equal(level(p, 'lane.signal'), 'unknown');
    assert.equal(level(p, 'lane.drift'), 'unknown');
    assert.equal(level(p, 'lane.noise'), 'unknown');
  });
  test('saved reference: a failed / not saved calibration wins; else LANE_STATUS.savedReference; else the session outcome', () => {
    const cal = (result: LaneCalibration['result'], timedOut = false): LaneCalibration => ({ result, reference: 3000, noise: 9, sysUs: 1, bootId: 1, timedOut });
    const id = 'lane.savedReference';
    const msg = (p: Partial<HealthInputs>): string => findCheck(evaluate(inputs(p)), id, 1)!.message;
    // the last calibration of this session failed or did not reach flash
    assert.equal(level(withCal(1, cal('failed')), id), 'fail');
    assert.match(msg(withCal(1, cal('failed', true))), /timed out/);
    assert.equal(level(withCal(1, cal('notSaved')), id), 'warn');
    // LANE_STATUS decides: running on the saved value ok, nothing saved warn, another value warn
    assert.equal(level(withCal(1, null), id), 'ok');
    assert.equal(level(withCal(1, cal('forgotten')), id), 'ok');                 // a later save wins over the outcome
    assert.equal(level(withLane(1, { savedReference: 0 }), id), 'warn');
    assert.match(msg(withLane(1, { savedReference: 0 })), /nothing saved/);
    assert.equal(level(withLane(1, { savedReference: 3500 }), id), 'warn');
    assert.match(msg(withLane(1, { savedReference: 3500 })), /recalibrate/);
    // no LANE_STATUS yet: the session outcome, else unknown
    const noLs = (c: LaneCalibration | null): Partial<HealthInputs> => ({ ...withCal(1, c), laneStatus: [null, null, laneStatus(2)] });
    const lvl = (p: Partial<HealthInputs>): CheckLevel => findCheck(evaluate(inputs(p)), id, 1)!.level;
    assert.equal(lvl(noLs(null)), 'unknown');
    assert.equal(lvl(noLs(cal('saved'))), 'ok');
    assert.equal(lvl(noLs(cal('forgotten'))), 'warn');
  });
  test('signal level: 1000 ok, 999 warn, 400 warn, 399 fail', () => {
    const at = (b: number): CheckLevel => level(withLane(1, { baseline: b, reference: b, thrLow: Math.round(b * 0.7), rawMax: b + 10 }), 'lane.signal');
    assert.deepEqual([1000, 999, 400, 399].map(at), ['ok', 'warn', 'warn', 'fail']);
  });
  test('saturation (rawMax or baseline): 59999 ok, 60000 warn, 64999 warn, 65000 fail', () => {
    assert.deepEqual([59999, 60000, 64999, 65000].map((v) => level(withLane(1, { rawMax: v }), 'lane.signal')), ['ok', 'warn', 'warn', 'fail']);
    assert.equal(level(withLane(1, { baseline: 65535, reference: 65535, thrLow: 45000, rawMax: 65535 }), 'lane.signal'), 'fail');
    assert.match(findCheck(evaluate(inputs(withLane(1, { rawMax: 65535 }))), 'lane.signal', 1)!.message, /IR current/);
  });
  test('drift baseline/reference: 0.85 ok, 0.849 warn, 0.70 warn, 0.699 fail, 1.20 ok, 1.201 warn, no reference unknown', () => {
    const at = (b: number): CheckLevel => level(withLane(1, { reference: 10000, baseline: b, thrLow: Math.round(b * 0.7) }), 'lane.drift');
    assert.deepEqual([8500, 8490, 7000, 6990, 12000, 12010].map(at), ['ok', 'warn', 'warn', 'fail', 'ok', 'warn']);
    assert.equal(level(withLane(1, { reference: 0 }), 'lane.drift'), 'unknown');
    assert.match(findCheck(evaluate(inputs(withLane(1, { reference: 10000, baseline: 6000, thrLow: 4200 }))), 'lane.drift', 1)!.message, /clean the strip/);
  });
  test('noise / margin: 0.199 ok, 0.2 warn, 0.329 warn, 0.33 fail', () => {
    // margin = baseline - thrLow = 1000
    assert.deepEqual([199, 200, 329, 330].map((n) => level(withLane(1, { noise: n }), 'lane.noise')), ['ok', 'warn', 'warn', 'fail']);
  });
  test('blocked now → warn', () => {
    assert.equal(level(withLane(1, { flags: GOOD_FLAGS | LaneStatusFlag.BLOCKED }), 'lane.blocked'), 'warn');
  });
  test('dropped events: 1 → warn, FIFO_OVERFLOW alone → warn; bad writes 1 → warn; late actions 1 → warn', () => {
    assert.equal(level(withLane(1, { droppedEvents: 1 }), 'lane.droppedEvents'), 'warn');
    assert.equal(level(withLane(1, { flags: GOOD_FLAGS | LaneStatusFlag.FIFO_OVERFLOW }), 'lane.droppedEvents'), 'warn');
    assert.equal(level(withLane(1, { badWrites: 1 }), 'lane.badWrites'), 'warn');
    assert.equal(level(withLane(1, { lateActions: 1 }), 'lane.lateActions'), 'warn');
  });
  test('reset cause: WATCHDOG / BROWNOUT / FAULT warn, SOFTWARE ok', () => {
    for (const c of [ResetCause.WATCHDOG, ResetCause.BROWNOUT, ResetCause.FAULT]) assert.equal(level(withLane(1, { resetCause: c }), 'lane.reset'), 'warn');
    assert.equal(level(withLane(1, { resetCause: ResetCause.SOFTWARE }), 'lane.reset'), 'ok');
  });
  test('a stale LANE_STATUS (race: slow poll) keeps its level and says how old it is', () => {
    const c = findCheck(evaluate(inputs({ laneAgeMs: [null, 20_000, 50] })), 'lane.sensor', 1)!;
    assert.equal(c.level, 'ok');
    assert.match(c.message, /20 s old/);
  });
  test('online lane without a LANE_STATUS yet: sample checks unknown', () => {
    const r = evaluate(inputs({ laneStatus: [] }));
    assert.equal(findCheck(r, 'lane.present', 1)!.level, 'ok');
    assert.equal(findCheck(r, 'lane.drift', 1)!.level, 'unknown');
    assert.equal(findCheck(r, 'lane.savedReference', 1)!.level, 'ok');      // from the event
  });
});

describe('node model (0.6)', () => {
  test('applyStatus: bridge state and seqRunning; EVT_BOOT keeps the bridge state, drops calibrations', () => {
    let m = applyStatus(emptyNodeModel(), status({ bleBridge: BleBridgeState.ABSENT, seqRunning: 1 }));
    assert.deepEqual([m.bleBridge, m.seqRunning], ['absent', true]);
    m = applyStatus(m, status({ bleBridge: BleBridgeState.VERSION_MISMATCH }));
    assert.equal(m.bleBridge, 'versionMismatch');
    const same = applyStatus(m, status({ bleBridge: BleBridgeState.VERSION_MISMATCH }));
    assert.equal(same, m);
    const n = nodes();
    assert.equal(n.lanes[1]!.calibration?.result, 'saved');
    const b = applyNodeEvent(applyStatus(n, status()), { bootId: 2, evSeq: 0, msgId: MsgId.EVT_BOOT, name: 'EVT_BOOT', value: {} } as unknown as DeviceEvent);
    assert.equal(b.bleBridge, 'ok');
    assert.equal(b.lanes[1]!.calibration, null);
  });
  test('fault recovery: CONFIGURED event or NODE_LIST OK takes FAULT → configured and clears the fault', () => {
    const f = applyNodeEvent(nodes(), ev(MsgId.EVT_LANE_STATE, { lane: 1, state: LaneState.FAULT, status: -5, sysUs: 9n }));
    assert.deepEqual([f.lanes[1]!.phase, f.lanes[1]!.faultStatus], ['fault', -5]);
    const a = applyNodeEvent(f, ev(MsgId.EVT_LANE_STATE, { lane: 1, state: LaneState.CONFIGURED, status: 0, sysUs: 10n }));
    assert.deepEqual([a.lanes[1]!.phase, a.lanes[1]!.faultStatus], ['configured', 0]);
    const b = applyNodeList(f, [node(NodeClass.LANE, 1, NodeState.OK)]);
    assert.deepEqual([b.lanes[1]!.phase, b.lanes[1]!.faultStatus], ['configured', 0]);
    const c = applyNodeList(f, [node(NodeClass.LANE, 1, NodeState.FAULT)]);
    assert.equal(c.lanes[1]!.phase, 'fault');
  });
  test('EVT_LANE_CALIBRATED: every result maps', () => {
    const r = [CalibrateResult.SAVED, CalibrateResult.FORGOTTEN, CalibrateResult.FAILED, CalibrateResult.NOT_SAVED].map((result) =>
      applyNodeEvent(emptyNodeModel(), ev(MsgId.EVT_LANE_CALIBRATED, { lane: 3, result, reference: 1, noise: 2, sysUs: 3n })).lanes[3]!.calibration!.result);
    assert.deepEqual(r, ['saved', 'forgotten', 'failed', 'notSaved']);
  });
});
