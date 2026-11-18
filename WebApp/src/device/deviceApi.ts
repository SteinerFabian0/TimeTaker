/*
 * A typed method for every protocol 0.6 request, on top of any Requester
 * (DeviceSession, or a fake in tests).
 *
 * - Lanes are numbers 1..6; masks are built with laneMask([1, 3]).
 * - Idempotent requests are retried on a timeout (a lost frame on BLE);
 *   START_SEQUENCE and REBOOT are not, because a retry could act twice.
 * - Errors: DeviceError (status name), TimeoutError, LinkClosedError.
 */

import {
  ABORT_SEQUENCE_REQ, ABORT_SEQUENCE_RSP, BIND_ALLOW_REQ, BIND_ALLOW_RSP, BIND_LIST_REQ, BIND_LIST_RSP,
  BIND_REMOVE_REQ, BIND_REMOVE_RSP, LANE_CALIBRATE_REQ, LANE_CALIBRATE_RSP,
  LANE_CONFIG_GET_REQ, LANE_CONFIG_GET_RSP, LANE_CONFIG_SET_REQ, LANE_CONFIG_SET_RSP, LANE_LED_REQ, LANE_LED_RSP,
  LANE_POWER_REQ, LANE_POWER_RSP, LANE_RAW_STREAM_REQ, LANE_RAW_STREAM_RSP, LANE_SAMPLING_REQ, LANE_SAMPLING_RSP,
  LANE_SCAN_REQ, LANE_SCAN_RSP, LANE_STATUS_REQ, LANE_STATUS_RSP, LedMode, MsgId, NODE_LIST_REQ, NODE_LIST_RSP,
  PING_REQ, PING_RSP, REBOOT_REQ, REBOOT_RSP, RebootTarget, SETTINGS_GET_REQ, SETTINGS_GET_RSP, SETTINGS_SET_REQ,
  SETTINGS_SET_RSP, START_SEQUENCE_REQ, START_SEQUENCE_RSP, STATUS_REQ, STATUS_RSP,
} from '../protocol/generated.ts';
import type {
  BindEntry, DeviceSettings, LaneConfig, LaneStatusRsp, NodeInfo, PingRsp, StatusRsp,
} from '../protocol/generated.ts';
import type { StructDesc } from '../protocol/wire.ts';
import { CalibrateAction, Status } from '../protocol/generated.ts';
import { DeviceError, TimeoutError } from './deviceSession.ts';
import type { RequestOptions, Requester } from './deviceSession.ts';

export const ALL_LANES = 0x3f;

/** Lane numbers (1..6) → laneMask bits. */
export function laneMask(lanes: Iterable<number>): number {
  let m = 0;
  for (const l of lanes) {
    if (!Number.isInteger(l) || l < 1 || l > 6) throw new RangeError(`lane ${l} is not 1..6`);
    m |= 1 << (l - 1);
  }
  return m;
}

/** laneMask bits → lane numbers. */
export function lanesOf(mask: number): number[] {
  const out: number[] = [];
  for (let l = 1; l <= 6; l++) if (mask & (1 << (l - 1))) out.push(l);
  return out;
}

export interface LedRequest {
  mode: number;
  level?: number;
  blinkPeriodMs?: number;
  blinkOnPermille?: number;
  blinkCount?: number;
}

export interface StartParams {
  lights: number;
  stepMs: number;
  holdMinMs: number;
  holdMaxMs: number;
  armSampling?: boolean;
  /** 0 = the device default (>= 150 ms). */
  leadMs?: number;
  /** Attention pulse: off to full in this many ms; default 1000. */
  pulseRiseMs?: number;
  /** Off-on-off breathing cycles before the countdown; default 2, 0 = none. */
  attentionPulses?: number;
}

export interface BindListResult {
  windowS: number;
  entries: BindEntry[];
}

export interface DeviceApiOptions {
  /** Retries after a timeout for idempotent requests. */
  readonly retries?: number;
  /** Waits before a retry after ERR_BUSY (a deferred answer, e.g. LANE_STATUS, is still owed). */
  readonly sleep?: (ms: number) => Promise<void>;
  readonly busyRetryMs?: number;
}

export class DeviceApi {
  #req: Requester;
  #retries: number;
  #sleep: ((ms: number) => Promise<void>) | undefined;
  #busyRetryMs: number;

  constructor(requester: Requester, opts: DeviceApiOptions = {}) {
    this.#req = requester;
    this.#retries = opts.retries ?? 2;
    this.#sleep = opts.sleep;
    this.#busyRetryMs = opts.busyRetryMs ?? 100;
  }

  get requester(): Requester { return this.#req; }

  async #call<T>(reqDesc: StructDesc, req: object, msgId: number, rspDesc: StructDesc,
                 opts: { retry?: boolean } & RequestOptions = {}): Promise<T> {
    const tries = opts.retry === false ? 1 : this.#retries + 1;
    for (let i = 0; ; i++) {
      try {
        return await this.#req.request<T>(reqDesc, req, msgId, rspDesc, opts);
      } catch (err) {
        // A timed-out deferred request is still owed by the device: the repeat meets ERR_BUSY, so wait and repeat.
        const busy = err instanceof DeviceError && err.status === Status.ERR_BUSY;
        if (!(err instanceof TimeoutError || busy) || i + 1 >= tries) throw err;
        if (busy) await this.#sleep?.(this.#busyRetryMs);
      }
    }
  }

  // ---- session / system ----------------------------------------------------

  ping(hostMs = 0): Promise<PingRsp> {
    return this.#call(PING_REQ, { hostMs: hostMs >>> 0 }, MsgId.PING, PING_RSP);
  }

  async nodeList(): Promise<NodeInfo[]> {
    return (await this.#call<{ nodes: NodeInfo[] }>(NODE_LIST_REQ, {}, MsgId.NODE_LIST, NODE_LIST_RSP)).nodes;
  }

  status(): Promise<StatusRsp> {
    return this.#call(STATUS_REQ, {}, MsgId.STATUS, STATUS_RSP);
  }

  async settingsGet(): Promise<DeviceSettings> {
    return (await this.#call<{ settings: DeviceSettings }>(SETTINGS_GET_REQ, {}, MsgId.SETTINGS_GET, SETTINGS_GET_RSP)).settings;
  }

  async settingsSet(s: { deviceName: string; bleEnabled: boolean | number }): Promise<void> {
    await this.#call(SETTINGS_SET_REQ, {
      settings: { deviceName: s.deviceName, bleEnabled: s.bleEnabled ? 1 : 0, reserved: new Uint8Array(3) },
    }, MsgId.SETTINGS_SET, SETTINGS_SET_RSP);
  }

  /** The response goes out before the reset; the link then drops. Not retried.
   *  (REBOOT{BOOTLOADER} is reserved: v1 has no bootloader.) */
  async reboot(): Promise<void> {
    await this.#call(REBOOT_REQ, { target: RebootTarget.APP }, MsgId.REBOOT, REBOOT_RSP, { retry: false });
  }

  // ---- binding ---------------------------------------------------------------

  /** The whole bind list (pages of 5) and the pairing window. */
  async bindList(): Promise<BindListResult> {
    const entries: BindEntry[] = [];
    let windowS = 0;
    for (let start = 0; ; ) {
      const rsp = await this.#call<{ total: number; start: number; windowS: number; entries: BindEntry[] }>(
        BIND_LIST_REQ, { start }, MsgId.BIND_LIST, BIND_LIST_RSP);
      windowS = rsp.windowS;
      entries.push(...rsp.entries);
      start += rsp.entries.length;
      if (rsp.entries.length === 0 || start >= rsp.total) break;
    }
    return { windowS, entries };
  }

  async bindRemove(clientId: Uint8Array): Promise<void> {
    await this.#call(BIND_REMOVE_REQ, { clientId }, MsgId.BIND_REMOVE, BIND_REMOVE_RSP);
  }

  bindClear(): Promise<void> {
    return this.bindRemove(new Uint8Array(16));
  }

  /** Opens the BLE pairing window for windowS seconds; 0 closes it. */
  async bindAllow(windowS: number): Promise<void> {
    await this.#call(BIND_ALLOW_REQ, { windowS }, MsgId.BIND_ALLOW, BIND_ALLOW_RSP);
  }

  // ---- lanes -----------------------------------------------------------------

  async laneScan(): Promise<void> {
    await this.#call(LANE_SCAN_REQ, {}, MsgId.LANE_SCAN, LANE_SCAN_RSP);
  }

  /** Answers once switching is under way; returns `powered` (0 while the rail settles). */
  async lanePower(on: boolean): Promise<boolean> {
    return (await this.#call<{ powered: number }>(LANE_POWER_REQ, { on: on ? 1 : 0 }, MsgId.LANE_POWER, LANE_POWER_RSP)).powered === 1;
  }

  async laneConfigGet(lane: number): Promise<LaneConfig> {
    return (await this.#call<{ config: LaneConfig }>(LANE_CONFIG_GET_REQ, { lane }, MsgId.LANE_CONFIG_GET, LANE_CONFIG_GET_RSP)).config;
  }

  async laneConfigSet(mask: number, config: LaneConfig, save = false): Promise<void> {
    await this.#call(LANE_CONFIG_SET_REQ, { laneMask: mask, save: save ? 1 : 0, reserved: 0, config },
      MsgId.LANE_CONFIG_SET, LANE_CONFIG_SET_RSP);
  }

  async laneSampling(mask: number, mode: number): Promise<void> {
    await this.#call(LANE_SAMPLING_REQ, { laneMask: mask, mode }, MsgId.LANE_SAMPLING, LANE_SAMPLING_RSP);
  }

  async laneLed(mask: number, led: LedRequest): Promise<void> {
    await this.#call(LANE_LED_REQ, {
      laneMask: mask, mode: led.mode, level: led.level ?? (led.mode === LedMode.OFF ? 0 : 1000),
      blinkPeriodMs: led.blinkPeriodMs ?? 0, blinkOnPermille: led.blinkOnPermille ?? 0, blinkCount: led.blinkCount ?? 0,
    }, MsgId.LANE_LED, LANE_LED_RSP);
  }

  laneStatus(lane: number): Promise<LaneStatusRsp> {
    return this.#call(LANE_STATUS_REQ, { lane }, MsgId.LANE_STATUS, LANE_STATUS_RSP);
  }

  /** Starts STR_RAW_SAMPLES for one lane; lane 0 stops. */
  async laneRawStream(lane: number, decimation = 1): Promise<void> {
    await this.#call(LANE_RAW_STREAM_REQ, { lane, reserved: 0, decimation }, MsgId.LANE_RAW_STREAM, LANE_RAW_STREAM_RSP);
  }

  /**
   * LEARN (lanes free!) or FORGET the saved reference of the lanes in mask. The answer only acknowledges;
   * each present lane's outcome follows as EVT_LANE_CALIBRATED (HealthMonitor.calibrate() waits for them).
   */
  async laneCalibrate(mask: number, action: CalibrateAction = CalibrateAction.LEARN): Promise<void> {
    await this.#call(LANE_CALIBRATE_REQ, { laneMask: mask, action }, MsgId.LANE_CALIBRATE, LANE_CALIBRATE_RSP);
  }

  // ---- sequences ---------------------------------------------------------------

  /** Returns T0 (sysUs). Not retried: a retry would answer ERR_SEQ_RUNNING. */
  async startSequence(p: StartParams): Promise<bigint> {
    const rsp = await this.#call<{ t0Us: bigint }>(START_SEQUENCE_REQ, {
      lights: p.lights, armSampling: p.armSampling === false ? 0 : 1, stepMs: p.stepMs, holdMinMs: p.holdMinMs,
      holdMaxMs: p.holdMaxMs, leadMs: p.leadMs ?? 0,
      pulseRiseMs: p.pulseRiseMs ?? 1000, attentionPulses: p.attentionPulses ?? 2, reserved: 0,
    }, MsgId.START_SEQUENCE, START_SEQUENCE_RSP, { retry: false });
    return rsp.t0Us;
  }

  async abortSequence(): Promise<void> {
    await this.#call(ABORT_SEQUENCE_REQ, {}, MsgId.ABORT_SEQUENCE, ABORT_SEQUENCE_RSP, { retry: false });
  }
}
