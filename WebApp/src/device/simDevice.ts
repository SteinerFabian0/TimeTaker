/*
 * SimDevice: a simulated TimeTaker control module (+ lanes, nRF bridge, cars)
 * that speaks host protocol 0.6 byte for byte.
 *
 * It is how the web app is built and tested before the PCBs exist, and it is
 * the REFERENCE THE FIRMWARE IS COMPARED AGAINST: every behaviour below is
 * either taken from CONTROL_FIRMWARE_GUIDE §5 / BLE_FIRMWARE_GUIDE /
 * SYSTEM_ARCHITECTURE, or is an assumption marked [A..]. When the firmware
 * decides differently, change the firmware or change this file and its
 * header, never silently.
 *
 * ---------------------------------------------------------------------------
 * Sessions and transports (SYSTEM_ARCHITECTURE §7.4, CONTROL guide §5.4)
 *   - One session per transport, at most one USB and one BLE link at once.
 *     A second open of the same transport fails ("port busy" / "not
 *     advertising").
 *   - HELLO opens the session (any time, again and again). It answers
 *     ERR_PROTO_MISMATCH on another MAJOR. HELLO sets the event cursor to the
 *     log head: it never replays history; RESUME does.
 *   - Every other request before HELLO: ERR_NO_SESSION. Wrong payload
 *     length: ERR_BAD_LENGTH. Unknown msgId: ERR_UNSUPPORTED. Errors are
 *     status-only responses.
 *   - RESUME: ERR_BOOT_MISMATCH (other bootId), ERR_LOG_GAP (fromSeq
 *     overwritten), ERR_INVALID_ARG (fromSeq beyond the head).
 *   - The pump sends log[cursor..head] to every session that is open and not
 *     UNBOUND. A cursor that fell behind the ring skips to the oldest entry
 *     (the app sees the hole, RESUMEs, gets ERR_LOG_GAP).
 *   - Streams (STR_RAW_SAMPLES, DBG_LOG) go to every open, not-UNBOUND
 *     session; on BLE they are capped at 2000 B/s and dropped beyond.
 *   - USB delivers device→host bytes in 64-byte chunks after latencyMs; BLE
 *     in 244-byte notifications after bleLatencyMs (a connection interval).
 *
 * Binding (SYSTEM_ARCHITECTURE §9.5, CONTROL guide §5.4 policy table)
 *   - USB sessions are TRUSTED. A USB HELLO with clientKind WEBAPP and a
 *     non-zero clientId adds that id to the bind list (EVT_BIND_CHANGED ADDED)
 *     or touches its LRU stamp. clientKind TOOL never binds. [A1]
 *   - BLE HELLO: on the list → BOUND (touch); window open → add, BOUND;
 *     otherwise UNBOUND. UNBOUND serves only HELLO and PING (ERR_NOT_BOUND
 *     for the rest) and gets no events or streams. [A2]
 *   - The 60 s kick timer starts at the FIRST unbound HELLO of a BLE
 *     connection and is not restarted by repeated HELLOs (the app polls with
 *     HELLO while it waits). At expiry: EVT_BIND_CHANGED REJECTED, then the
 *     link drops (EVT_BLE_LINK DOWN, reason 0x16). [A3]
 *   - The window opening (BLE button LONG, BIND_ALLOW) binds an open
 *     UNBOUND session live: ADDED, session BOUND, kick timer cancelled. The
 *     window stays open for its full time after a bind. [A4]
 *   - BLE button VERY_LONG: EVT_BUTTON, then CLEARED, then WINDOW_CLOSED (if
 *     it was open); an open BLE session drops to UNBOUND (new kick timer).
 *   - BIND_ALLOW{0} closes the window (WINDOW_CLOSED only if it was open).
 *     Any other value opens or re-opens it for that many seconds, at most
 *     600 (firmware HOST_MANAGER_BIND_WINDOW_MAX_S). [A19]
 *   - The all-zero clientId is never a client (it is BIND_REMOVE's "clear"
 *     wildcard): a BLE HELLO with it stays UNBOUND even with the window open,
 *     and a live bind skips it. [A20]
 *   - A client evicted by the LRU rule loses its rights like a removed one: an
 *     open BLE session of that client drops to UNBOUND. [A21]
 *   - BIND_REMOVE of an id not on the list: OK, no event (retry-safe). [A5]
 *     Removing the id of the open BLE session drops it to UNBOUND.
 *   - The list holds 8. Adding a 9th evicts the least recently used:
 *     REMOVED (evicted id) then ADDED. [A6]
 *   - BIND_LIST pages 5 entries in list order; start > total:
 *     ERR_INVALID_ARG.
 *
 * Lanes (CONTROL guide §5.1)
 *   - LANE_POWER on: rail, SYNC after 30 ms; the first lane hot-joins 50 ms
 *     later (it boots), each next one 15 ms after the previous was addressed.
 *     Per lane: EVT_LANE_STATE JOINED, then 4 ms later CONFIGURED (config,
 *     SYNC_ALIGN and sampling mode pushed from the table). The answer's
 *     `powered` is 0 while the rail settles.
 *   - End of chain: the first lane that does not join ends enumeration
 *     silently. Fewer than six lanes is normal.
 *   - LANE_POWER off: LEFT for every enumerated lane (index order), a running
 *     sequence aborts (ABORT_POWER), the raw stream stops.
 *   - A broken chain (unplugLane n) or a lane reset is seen within 30 ms
 *     (three failed 10 ms polls): LEFT for lane n and every lane after it.
 *     A reset lane re-joins after its 50 ms boot, and the rest follow in
 *     chain order, configured and sampling again without host action.
 *   - LANE_SCAN needs the rail (ERR_INVALID_STATE otherwise): LEFT for all,
 *     then the chain re-joins in order. Lanes do not reset on a scan.
 *   - laneSeq restarts at 0 only when that lane resets (power, reset), not
 *     on re-enumeration. [A7]
 *   - Config table: one entry per index with compiled defaults
 *     (samplePeriodUs 1000, phase (n-1)*160 µs, mergeGap 30 ms, block
 *     timeout 500 ms, holdoff 0, thresholds 300/100 ‰, minDwell 2 ms,
 *     raw decimation 1, IR 7/3/0). LANE_CONFIG_SET writes the table and
 *     pushes to present lanes without an event [A8]; save = 1 also persists
 *     it (survives reboots). LANE_CONFIG_GET works for absent lanes.
 *   - Sampling mode is part of the RAM table, default ON [A9] (not
 *     persisted), restored after every re-join. RAW via LANE_SAMPLING is
 *     allowed (detects, no stream without LANE_RAW_STREAM).
 *   - Mask requests (CONFIG_SET, SAMPLING, LED, CALIBRATE): mask 0 or above
 *     0x3f → ERR_INVALID_ARG. LED and CALIBRATE act on the present lanes in
 *     the mask and answer ERR_LANE_ABSENT if none is present. [A10]
 *   - LANE_STATUS / LANE_RAW_STREAM on an absent lane: ERR_LANE_ABSENT;
 *     lane outside 1..6: ERR_INVALID_ARG.
 *   - LANE_RAW_STREAM: one lane at a time; starting another stops the first
 *     and restores its mode; lane 0 stops. A chunk every 50 ms, at most 112
 *     samples (decimated) per chunk, chunkSeq counts every chunk built.
 *
 * Crossings (SYSTEM_ARCHITECTURE §4, LANE guide §3.5)
 *   - Detected only while the rail is on, the lane is CONFIGURED and its
 *     sampling mode is ON or RAW. tEntryUs is the nose on the line. The
 *     event is published on exit + merge gap + up to one 10 ms poll, so
 *     events are NOT in tEntryUs order across lanes and a crossing just
 *     before lights out can be logged after EVT_LIGHTS_OUT.
 *   - IN_HOLDOFF: holdoffUs > 0 and less than holdoffUs since the lane's
 *     last CROSSING that was not itself flagged (lane_protocol.h: flagged
 *     crossings and BLOCKED do not restart the window). [A11]
 *   - A car standing on the line: BLOCKED (tEntryUs = entry, dwell = the
 *     block timeout) after the block timeout, then CLEARED (tEntryUs = the
 *     same entry, as lane_protocol.h says; dwell = standing time, capped at
 *     655 ms) — no CROSSING.
 *   - After SYNC_LOST a lane's crossings carry EXTRAPOLATED until SYNC_OK.
 *
 * Start sequence (CONTROL guide §5.3)
 *   - Checks in order: running → ERR_SEQ_RUNNING; lights 0 or > 6, step 0,
 *     holdMin > holdMax, armSampling > 1 → ERR_INVALID_ARG; 0 < leadMs < 150
 *     → ERR_TOO_LATE; no lane present → ERR_LANE_ABSENT; a lane that does not
 *     take its actions → ERR_LANE_NACK (nothing published). [A12]
 *   - n = min(lights, lanes present), the first n present lanes. P = pulseRiseMs
 *     (0 = 1000), k = attentionPulses. From T0 every lit lane breathes
 *     (level = (1 - cos(pi t / P)) / 2, in phase; laneLed() reports it as ON
 *     with that level). The far-end lane (highest index of the n) goes ON at
 *     the last peak T0 + (2k-1)P, the others OFF at the trough T0 + 2kP; then
 *     lanes far-1 .. 1 go ON at T0 + 2kP + j·step. k = 0: far lane on at T0,
 *     the others every step. Tlit = T0 + 2kP + (n-1)·step; hold uniform in
 *     [holdMin, holdMax] (µs resolution); Tout = Tlit + hold, all OFF. Both events carry n as `lights`. [A13]
 *   - EVT_SEQ_STARTED{t0Us} is published before the response. EVT_LIGHTS_OUT
 *     {toutUs} is published at Tout plus up to 0.8 ms (a loop pass), never
 *     earlier. "Running" ends at Tout.
 *   - armSampling: at T0 every present lane's table mode becomes ON (if it
 *     was OFF), so a re-join keeps it. [A14]
 *   - LANE_LED while running: ERR_SEQ_RUNNING.
 *   - ABORT_SEQUENCE when idle: ERR_INVALID_STATE. Abort (host, BTN2 LONG,
 *     lane LEFT, lane power off): lights blink 3 times, EVT_SEQ_ABORTED
 *     {reason, atUs}.
 *
 * System
 *   - REBOOT{APP}: response first, reset 100 ms later. USB links
 *     close and the port is gone for 400 ms (re-enumeration). A BLE link
 *     survives an STM reset (the nRF is not reset): its session is gone, so
 *     the next request gets ERR_NO_SESSION until a new HELLO; the new log
 *     starts EVT_BOOT, then EVT_BLE_LINK UP. [A15]
 *   - After any boot the lane rail is off. Settings, the bind list and the
 *     saved lane table persist; the pairing window and sampling modes do not.
 *   - STATUS: lastError / errorCount count faults (EVT_FAULT, lane FAULT),
 *     not error responses. [A16]
 *   - SETTINGS_SET: bleEnabled > 1 → ERR_INVALID_ARG. bleEnabled 0 drops a
 *     BLE link and stops advertising. Flash writes are deferred: at most one
 *     per 10 s and never while a sequence runs (settingsWrites counts them).
 *   - BLE button presses are published as EVT_BUTTON{BLE_BIND}. BTN2 LONG on
 *     the panel aborts a running sequence (reason BUTTON).
 *
 * Lane signal and calibration (0.6; lane_protocol.h "Reference", lane_detect.h)
 *   - Each lane's optical signal is a pure function of (seed, lane, time):
 *     bare-foil level (3000 + 85·lane counts) × a slow ambient drift
 *     (±1.5 %, 5 min period) × dirt, minus the car passes (depth × 2.2),
 *     plus Gaussian noise (6 + lane counts), sampled every samplePeriodUs and
 *     clipped to 0..65535. LANE_STATUS polls and raw streams do not consume
 *     the simulator's RNG, so they cannot change a recorded race. [A22]
 *   - At every join: a saved reference (control module flash, per lane
 *     INDEX) is pushed as SET_REFERENCE: baseline = reference = saved,
 *     CALIBRATED at once. Without one the lane learns for 64 samples
 *     (DETECT_LEARN_SAMPLES) and reference = the learned level. The baseline
 *     then tracks the bare level with a 2.048 s time constant
 *     (DETECT_BASELINE_SHIFT 11); the reference does not move.
 *   - LANE_STATUS flags: SYNC_LOCKED|SYNC_ALIGNED (configured, SYNC ok),
 *     SYNC_LOST, SENSOR_OK, CONFIG_VALID, CALIBRATED (not learning), BLOCKED
 *     (raw below thrLow now), FIFO_OVERFLOW (droppedEvents > 0).
 *     rawMin/rawMax: min/max of every sample since the previous LANE_STATUS
 *     of that lane (or its join); a read resets the window. Both = raw when
 *     no sample was taken since.
 *   - LANE_CALIBRATE while a start sequence runs: ERR_SEQ_RUNNING.
 *   - LEARN: each CONFIGURED lane in the mask learns for 100 ms (mean of the
 *     samples: a car passing spoils it, lanes must be free; sampling OFF
 *     does not matter). Other masked lanes are skipped silently; no
 *     configured lane in the mask: ERR_LANE_ABSENT. The lane uses the new
 *     reference at once (LANE_STATUS shows it, rawMin/rawMax window reset);
 *     EVT_LANE_CALIBRATED SAVED follows the deferred settings write (at once
 *     after an idle period, up to 10 s later after a recent write, never
 *     during a sequence). Flash write failing: NOT_SAVED (used until
 *     power-off). The lane leaving the bus while it learns, or a broken
 *     sensor: FAILED, reference 0, at once. A second LEARN of a lane still
 *     learning restarts it (one event). [A23]
 *   - FORGET: every masked lane INDEX, present or not, never
 *     ERR_LANE_ABSENT; FORGOTTEN (reference 0) per masked lane after the
 *     settings write (NOT_SAVED, reference 0, if it fails). Present lanes
 *     re-learn as at boot at once. [A24]
 *   - A faulted lane that recovers publishes EVT_LANE_STATE CONFIGURED.
 *     A broken sensor (sensorFault) is a lane FAULT (ERR_HAL), SENSOR_OK
 *     clear, raw 0; it stays broken across re-joins until repaired.
 *
 * BLE bridge (0.6 STATUS)
 *   - bleBridge: OK while the nRF heartbeats (STATS every second), ABSENT
 *     3 s after it went silent (nrfAlive(false)) or when no module is fitted,
 *     VERSION_MISMATCH by knob. A silent or mismatched nRF accepts no BLE
 *     connection; a phone connected when the nRF dies is gone at once, and
 *     the control module publishes EVT_BLE_LINK DOWN (supervision timeout)
 *     when it declares the bridge ABSENT. [A25]
 *   - bleRssi: the knob value (default -58 dBm) while a phone is connected
 *     and the bridge is OK with a STATS received (1 s after boot), 127
 *     otherwise. bleCrcErrors / bleUartErrors / bleDataDropped (nRF STATS,
 *     0 before the first STATS) / nrfLinkCrcErrors: knob values,
 *     saturating at 65535.
 *   - settingsDefaults: 1 after a boot that found the settings corrupt
 *     (corruptSettings()): settings, bind list, lane table and references
 *     back to defaults, EVT_FAULT{SETTINGS, ERR_SETTINGS_CORRUPT} after
 *     EVT_BOOT.
 *
 * No bootloader (protocol 0.4, SYSTEM_ARCHITECTURE §10): v1 flashes the
 *   control module over SWD. REBOOT{BOOTLOADER} → ERR_UNSUPPORTED; the old
 *   update ids (0xA0–0xAF) are unknown requests → ERR_UNSUPPORTED. NODE_LIST
 *   reports the CTRL imageCrc as 0 (unknown), like the firmware. [A17]
 *
 * Test hooks: unplug/replug USB, BLE out of range, unplug/replug/reset a
 * lane, SYNC loss, lane fault and recovery, lost lane events, dropped event
 * frames, failing lane schedules, faults, buttons, power cycle, scripted
 * cars, and the ground truth of every car crossing (detected or not).
 * Diagnostics knobs (0.6): nRF silent / version mismatch, RSSI and link
 * counters, loop time, corrupt settings, failing flash writes, and per lane
 * dirty strip, noisy sensor, saturated sensor, broken sensor.
 */

import { FrameDecoder, frameEncode } from '../protocol/codec.ts';
import { decodePacket, encodePacket } from '../protocol/packet.ts';
import {
  ABORT_SEQUENCE_RSP, BIND_ALLOW_REQ, BIND_ALLOW_RSP, BIND_LIST_REQ, BIND_LIST_RSP, BIND_REMOVE_REQ, BIND_REMOVE_RSP,
  BindChange, BindState, BleBridgeState, BleLinkState, BoardType, ButtonId, CalibrateAction, CalibrateResult, ClientKind,
  DBG_LOG, EVT_BIND_CHANGED, EVT_BLE_LINK, EVT_BOOT, EVT_BUTTON, EVT_CROSSING, EVT_FAULT, EVT_LANE_CALIBRATED, EVT_LANE_STATE, EVT_LIGHTS_OUT, EVT_SEQ_ABORTED,
  EVT_SEQ_STARTED, FaultSource, HELLO_REQ, HELLO_RSP, HeaderFlag, LANE_CALIBRATE_REQ, LANE_CALIBRATE_RSP,
  LANE_CONFIG_GET_REQ, LANE_CONFIG_GET_RSP, LANE_CONFIG_SET_REQ, LANE_CONFIG_SET_RSP, LANE_LED_REQ, LANE_LED_RSP,
  LANE_POWER_REQ, LANE_POWER_RSP, LANE_RAW_STREAM_REQ, LANE_RAW_STREAM_RSP, LANE_SAMPLING_REQ, LANE_SAMPLING_RSP,
  LANE_SCAN_RSP, LANE_STATUS_REQ, LANE_STATUS_RSP, LaneBusState, LaneEventFlag, LaneEventType, LaneState, LedMode,
  LogLevel, MESSAGES, MsgId, NODE_LIST_RSP, NodeClass, NodeState, PING_REQ, PING_RSP, PressKind, PROTO, REBOOT_REQ,
  REBOOT_RSP, RebootTarget, ResetCause, RESUME_REQ, RESUME_RSP, SamplingMode, SeqAbortReason, SETTINGS_GET_RSP,
  SETTINGS_SET_REQ, SETTINGS_SET_RSP, START_SEQUENCE_REQ, START_SEQUENCE_RSP, Status, STATUS_RSP, STR_RAW_SAMPLES,
  Transport as HpTransport,
} from '../protocol/generated.ts';
import type { BindEntry, LaneConfig, NodeInfo } from '../protocol/generated.ts';
import { decodeStruct, encodeStruct } from '../protocol/wire.ts';
import type { StructDesc } from '../protocol/wire.ts';
import { SimTransport } from '../transport/simTransport.ts';
import type { SimLinkHost } from '../transport/simTransport.ts';
import type { Transport, TransportHandlers } from '../transport/transport.ts';
import { realScheduler } from '../util/clock.ts';
import type { Scheduler, TimerHandle } from '../util/clock.ts';
import { bytesEqual, isZero } from '../util/bytes.ts';
import { makeRng, subSeed } from '../util/rng.ts';
import type { Rng } from '../util/rng.ts';
import { LaneStatusFlag } from './laneFlags.ts';
import { DEFAULT_CAR, SimCar } from './sim/simCars.ts';
import type { CarProfile, CarScript, CrossingDetail, CrossingKind } from './sim/simCars.ts';

export type { CarProfile, CarScript, CrossingKind } from './sim/simCars.ts';

export interface SimOptions {
  /** USB one-way delay, ms. */
  readonly latencyMs?: number;
  /** BLE one-way delay, ms (about one connection interval). */
  readonly bleLatencyMs?: number;
  /** USB device → host chunk size. */
  readonly chunkBytes?: number;
  /** Event log size (the firmware has 1600, HOST_MANAGER_EVENT_LOG_CAPACITY). */
  readonly logCapacity?: number;
  /** Lanes on the chain (0..6). */
  readonly lanes?: number;
  /** Cars lap on powered lanes from the start (free practice). */
  readonly simulateCars?: boolean;
  /** Fixed seed for repeatable runs; random otherwise. */
  readonly seed?: number;
  readonly scheduler?: Scheduler;
  /** The nRF bridge is fitted. */
  readonly bleModule?: boolean;
  /** Unbound BLE grace time (60 s). */
  readonly unboundKickMs?: number;
  /** Per-lane car profiles (lane 1..6), merged over DEFAULT_CAR and the lane's own pace. */
  readonly cars?: Readonly<Record<number, Partial<CarProfile>>>;
}

/** One car crossing as it really happened: the simulator's ground truth. */
export interface TruthCrossing {
  readonly lane: number;
  readonly bootId: number;
  readonly tEntryUs: number;
  readonly kind: CrossingKind;
  /** false when the lane could not see it (unpowered, absent, sampling off). */
  readonly detected: boolean;
}

export interface SimEvent {
  readonly evSeq: number;
  readonly msgId: number;
  readonly name: string;
  readonly value: Record<string, unknown>;
}

interface Session {
  clientId: Uint8Array;
  clientName: string;
  clientKind: number;
  bindState: number;
  cursor: number;
}

interface Link {
  readonly kind: 'usb' | 'ble';
  readonly handlers: TransportHandlers;
  readonly decoder: FrameDecoder;
  session: Session | null;
  open: boolean;
  kickTimer: TimerHandle | null;
  /** BLE stream budget: bytes allowed, refilled over time. */
  budget: number;
  budgetAtMs: number;
}

interface LedState {
  mode: number; level: number; blinkPeriodMs: number; blinkOnPermille: number; blinkCount: number;
  /** LANE_ACT_LED_PULSE: level(t) = (1 - cos(pi t / rise)) / 2 from startUs (sysUs); resolved by laneLed(). */
  pulse?: { startUs: number; riseMs: number };
}

interface LaneSlot {
  readonly index: number;
  present: boolean;
  configured: boolean;
  laneSeq: number;
  lastEventUs: number | null;
  bootMs: number;
  resetCause: number;
  led: LedState;
  syncOk: boolean;
  fault: number;
  /** Tracked baseline (lane_detect): null until learned. atUs = sysUs of the last update. */
  base: { val: number; atUs: number } | null;
  /** Saved/learned clean level (0 = none yet). */
  reference: number;
  /** Learning in progress (boot learn or LEARN): ends at untilUs (sysUs). */
  learning: { gen: number; untilUs: number; fromUs: number; report: boolean } | null;
  learnGen: number;
  /** rawMin/rawMax window start (sysUs). */
  winFromUs: number;
  passes: { startUs: number; endUs: number; depth: number }[];
  droppedEvents: number;
  syncShifts: number;
  badWrites: number;
  lateActions: number;
}

/** The physical lane: survives reboots and re-joins (sim knobs). */
interface LaneHw {
  /** Bare-foil level (counts) with a clean strip. */
  bare0: number;
  /** Noise standard deviation (counts). */
  noiseSd: number;
  /** Dirty strip: bare level × dirt (1 = clean). */
  dirt: number;
  /** Noisy sensor: noise × this. */
  noiseFactor: number;
  /** IR current too high for the foil: the sensor clips at 65535. */
  saturated: boolean;
  /** The VCNL4040 does not answer. */
  sensorBroken: boolean;
}

interface BindRec { clientId: Uint8Array; clientName: string; lastUsed: number }

interface Firmware { major: number; minor: number; patch: number; gitRev: number }

interface Flash {
  settings: { deviceName: string; bleEnabled: number };
  binds: BindRec[];
  laneTable: LaneConfig[];
  /** Saved lane references by lane index (0 = none). */
  references: number[];
}

interface Sequence {
  t0Us: number;
  toutUs: number;
  lanes: number[];
  timers: TimerHandle[];
}

const LANE_SETTLE_MS = 30;
const LANE_BOOT_MS = 50;
const LANE_JOIN_STEP_MS = 15;
const CONFIG_PUSH_MS = 4;
const LEFT_DETECT_MS = 30;
const POLL_MS = 10;
const SEQ_MIN_LEAD_MS = 150;
const SEQ_DEFAULT_PULSE_MS = 1000;
const REBOOT_DELAY_MS = 100;
const USB_REENUM_MS = 400;
const BIND_WINDOW_S = 60;
const BIND_MAX = 8;
const BIND_WINDOW_MAX_S = 600;
const BIND_PAGE = 5;
const SETTINGS_MIN_INTERVAL_MS = 10000;
const RAW_CHUNK_MS = 50;
const RAW_MAX_SAMPLES = 112;
const BLE_STREAM_BYTES_PER_S = 2000;
const HCI_REMOTE_USER = 0x13;
const HCI_LOCAL_HOST = 0x16;
const HCI_SUPERVISION_TIMEOUT = 0x08;
const MAX_LANES = 6;
/** lane_detect.h DETECT_LEARN_SAMPLES (boot learn, FORGET). */
const LEARN_BOOT_SAMPLES = 64;
/** LANE_CALIBRATE LEARN duration. */
const LEARN_MS = 100;
/** Baseline tracking time constant: 2^DETECT_BASELINE_SHIFT samples of 1 ms. */
const BASELINE_TAU_US = 2_048_000;
/** Ambient drift: amplitude and period. */
const DRIFT_AMPLITUDE = 0.015;
const DRIFT_PERIOD_MS = 300_000;
/** Dip of a passing car per unit of depth. */
const DIP_PER_DEPTH = 2.2;
/** At most this many samples are evaluated for one rawMin/rawMax window (longer windows are strided). */
const WINDOW_MAX_EVAL = 4000;
/** The nRF heartbeats every second; ABSENT after 3 s of silence (bleBridgeState doc). */
const NRF_SILENT_MS = 3000;
const RSSI_UNKNOWN = 127;

export const SIM_APP_FW: Firmware = { major: 0, minor: 2, patch: 0, gitRev: 0x51d0052 };
const SIM_LANE_FW: Firmware = { major: 0, minor: 1, patch: 0, gitRev: 0x1a9e000 };
const SIM_BLE_FW: Firmware = { major: 0, minor: 1, patch: 0, gitRev: 0xb1e0000 };
/** NODE_LIST protoMinor: lane_protocol.h LANE_PROTO_MINOR and ble_link_protocol.h BLE_LINK_MINOR. */
const SIM_LANE_PROTO_MINOR = 3;
const SIM_BLE_LINK_MINOR = 2;

export function defaultLaneConfig(lane: number): LaneConfig {
  return {
    samplePeriodUs: 1000, samplePhaseUs: (lane - 1) * 160, mergeGapUs: 30000, blockTimeoutUs: 500000,
    holdoffUs: 0, holdoffSamplePeriodUs: 0, thrDropPermille: 300, hystPermille: 100, minDwell10us: 200,
    rawDecimation: 1, irCurrent: 7, psIntegration: 3, psMultiPulse: 0, reserved: 0,
  };
}

const sat16 = (n: number): number => Math.max(0, Math.min(0xffff, Math.round(n)));

function defaultFlash(): Flash {
  return {
    settings: { deviceName: 'TimeTaker', bleEnabled: 1 },
    binds: [],
    laneTable: Array.from({ length: MAX_LANES + 1 }, (_, i) => defaultLaneConfig(Math.max(1, i))),
    references: new Array<number>(MAX_LANES + 1).fill(0),
  };
}

/** 32-bit integer hash (deterministic noise without consuming an RNG). */
function hash32(a: number, b: number, c: number): number {
  let h = (a ^ Math.imul(b, 0x9e3779b1) ^ Math.imul(c, 0x85ebca77)) >>> 0;
  h = Math.imul(h ^ (h >>> 16), 0x7feb352d);
  h = Math.imul(h ^ (h >>> 15), 0x846ca68b);
  return (h ^ (h >>> 16)) >>> 0;
}

/** Standard normal value for (seed, lane, sample index): Box-Muller over two hashes. */
function gauss(seed: number, lane: number, idx: number): number {
  const u1 = (hash32(seed, lane * 2 + 1, idx) + 1) / 4294967297;
  const u2 = hash32(seed, lane * 2 + 2, idx) / 4294967296;
  return Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
}

const BODY_ID = new Map<StructDesc, number>();
for (const m of MESSAGES) if (m.body) BODY_ID.set(m.body, m.id);

const LED_OFF: LedState = { mode: LedMode.OFF, level: 0, blinkPeriodMs: 0, blinkOnPermille: 0, blinkCount: 0 };

export class SimDevice implements SimLinkHost<Link> {
  readonly scheduler: Scheduler;
  readonly #opts: Required<Omit<SimOptions, 'scheduler' | 'cars'>>;
  #rng: Rng;
  #bootId = 0;
  #bootMs = 0;
  #log: ({ msgId: number; body: Uint8Array } | undefined)[] = [];
  #next = 0;
  #links = new Set<Link>();
  #pumpTimer: TimerHandle | null = null;
  #usbPlugged = true;
  #usbReadyAtMs = 0;
  #bleInRange = true;
  #flash: Flash;
  #lru = 0;
  #window: { untilMs: number; timer: TimerHandle } | null = null;
  #rail: 'off' | 'settling' | 'on' = 'off';
  #railTimer: TimerHandle | null = null;
  #chainLength: number;
  #chainBreak: number | null = null;
  #lanes: LaneSlot[] = [];
  #enumGen = 0;
  #laneTable: LaneConfig[] = [];
  #sampling: number[] = [];
  #seq: Sequence | null = null;
  #failSchedule = new Set<number>();
  #raw: { lane: number; decimation: number; prevMode: number; chunkSeq: number; fromUs: number; timer: TimerHandle } | null = null;
  #cars: SimCar[] = [];
  #truth: TruthCrossing[] = [];
  #errorCount = 0;
  #lastError = 0;
  #usbFaults = 0;
  #settingsWrites = 0;
  #settingsDirty = false;
  #lastSettingsWriteMs = -Infinity;
  #settingsTimer: TimerHandle | null = null;
  #dropEvents = 0;
  #timers = new Set<TimerHandle>();
  #disposed = false;
  #hw: LaneHw[] = [];
  #signalSeed: number;
  /** nRF: silentSinceMs = scheduler ms it stopped heartbeating (null = alive). */
  #nrf: { silentSinceMs: number | null; mismatch: boolean; downTimer: TimerHandle | null } = { silentSinceMs: null, mismatch: false, downTimer: null };
  #bleRssi = -58;
  #bleStats = { bleCrcErrors: 0, bleUartErrors: 0, bleDataDropped: 0, nrfLinkCrcErrors: 0 };
  #loopMaxMs = 2;
  #corruptAtNextBoot = false;
  #settingsDefaults = 0;
  #flashFails = false;
  /** Calibration outcomes waiting for the deferred settings write (SAVED / FORGOTTEN / NOT_SAVED follow it). */
  #pendingFlash = new Map<number, { action: number; reference: number }>();

  constructor(opts: SimOptions = {}) {
    this.scheduler = opts.scheduler ?? realScheduler;
    this.#opts = {
      latencyMs: 1, bleLatencyMs: 15, chunkBytes: 64, logCapacity: 1600, simulateCars: false, lanes: 6,
      seed: Math.floor(Math.random() * 0xffffffff), bleModule: true, unboundKickMs: 60000,
      ...Object.fromEntries(Object.entries(opts).filter(([k, v]) => v !== undefined && k !== 'scheduler' && k !== 'cars')),
    } as Required<Omit<SimOptions, 'scheduler' | 'cars'>>;
    this.#rng = makeRng(subSeed(this.#opts.seed, 1));
    this.#chainLength = Math.max(0, Math.min(MAX_LANES, this.#opts.lanes));
    this.#flash = defaultFlash();
    this.#signalSeed = subSeed(this.#opts.seed, 7);
    this.#hw = Array.from({ length: MAX_LANES + 1 }, (_, i) => ({
      bare0: 3000 + i * 85, noiseSd: 6 + i, dirt: 1, noiseFactor: 1, saturated: false, sensorBroken: false,
    }));
    const host = { scheduler: this.scheduler, onCross: (l: number, at: number, k: CrossingKind, d: CrossingDetail) => this.#onCross(l, at, k, d) };
    for (let lane = 1; lane <= MAX_LANES; lane++) {
      const carRng = makeRng(subSeed(this.#opts.seed, 100 + lane));
      const pace: CarProfile = { ...DEFAULT_CAR, lapMs: Math.round(carRng.range(4800, 6800)), lapSdMs: Math.round(carRng.range(60, 180)) };
      this.#cars[lane] = new SimCar(lane, carRng, host, { ...pace, ...(opts.cars?.[lane] ?? {}) });
    }
    this.#boot(ResetCause.POWER_ON);
    if (this.#opts.simulateCars) this.carsPractice();
  }

  // ---- observation ------------------------------------------------------------------

  get bootId(): number { return this.#bootId; }
  get logNextSeq(): number { return this.#next; }
  get logOldestSeq(): number { return this.#oldest(); }
  get lanesPowered(): boolean { return this.#rail === 'on'; }
  get lanesPresent(): number { return this.#presentCount(); }
  get seqRunning(): boolean { return this.#seq !== null; }
  get usbPlugged(): boolean { return this.#usbPlugged; }
  get settingsWrites(): number { return this.#settingsWrites; }
  get bindList(): readonly { clientId: Uint8Array; clientName: string; lastUsed: number }[] { return this.#flash.binds.map((b) => ({ ...b })); }
  get pairingWindowOpen(): boolean { return this.#window !== null; }
  get cars(): boolean { return this.#cars.slice(1).some((c) => c.state !== 'off'); }
  laneLed(lane: number): Readonly<LedState> {
    const led = this.#lanes[lane]?.led ?? LED_OFF;
    if (!led.pulse) return { ...led };
    const t = Math.max(0, this.#sysUsNow() - led.pulse.startUs);
    const level = Math.round(1000 * (1 - Math.cos(Math.PI * t / (led.pulse.riseMs * 1000))) / 2);
    return { mode: LedMode.ON, level, blinkPeriodMs: 0, blinkOnPermille: 0, blinkCount: 0 };
  }
  laneSamplingMode(lane: number): number { return this.#effectiveMode(lane); }
  laneConfig(lane: number): LaneConfig { return { ...this.#laneTable[lane]! }; }
  car(lane: number): SimCar { return this.#cars[lane]!; }
  /** Every car crossing so far, detected or not, in the order they happened. */
  truth(): readonly TruthCrossing[] { return this.#truth.slice(); }

  sysUs(): bigint { return BigInt(this.#sysUsNow()); }

  /** The retained event log, decoded (debugging, fixtures). */
  eventLog(): SimEvent[] {
    const out: SimEvent[] = [];
    for (let seq = this.#oldest(); seq < this.#next; seq++) {
      const e = this.#log[seq % this.#opts.logCapacity];
      if (!e) continue;
      const msg = MESSAGES.find((m) => m.id === e.msgId)!;
      const payload = new Uint8Array(4 + e.body.length);
      new DataView(payload.buffer).setUint32(0, seq, true);
      payload.set(e.body, 4);
      out.push({ evSeq: seq, msgId: e.msgId, name: msg.name, value: decodeStruct(msg.body!, payload) });
    }
    return out;
  }

  /** A transport to plug into a DeviceSession: one USB cable or one BLE connection to this device. */
  transport(kind: 'usb' | 'ble' | 'sim' = 'usb'): Transport {
    return new SimTransport<Link>(this, kind === 'ble' ? 'ble' : 'usb');
  }

  // ---- test hooks ---------------------------------------------------------------------

  pressButton(button: number, press: number): void {
    this.#publish(EVT_BUTTON, { button, press, reserved: 0, sysUs: this.#sysUsBig() });
    if (button === ButtonId.BTN2 && press === PressKind.LONG && this.#seq) this.#abortSequence(SeqAbortReason.BUTTON);
  }

  /** SW501 on the nRF (classified by the nRF, decided here). */
  pressBleButton(press: number): void {
    if (!this.#opts.bleModule) return;
    this.#publish(EVT_BUTTON, { button: ButtonId.BLE_BIND, press, reserved: 0, sysUs: this.#sysUsBig() });
    if (press === PressKind.LONG) this.#openWindow(BIND_WINDOW_S);
    else if (press === PressKind.VERY_LONG) this.#clearBinds(true);
  }

  /** Legacy switch: free practice on every lane, or all cars off. */
  setCars(on: boolean): void {
    if (on) this.carsPractice(); else this.carsStop();
  }

  carsPractice(lanes: readonly number[] = [1, 2, 3, 4, 5, 6]): void {
    for (const l of lanes) this.#cars[l]?.practice();
  }

  /** Cars wait on the grid for the next start sequence; each parks after `stopAfter` genuine crossings. */
  carsRace(lanes: readonly number[] = [1, 2, 3, 4, 5, 6], stopAfter = Number.POSITIVE_INFINITY): void {
    for (const l of lanes) this.#cars[l]?.grid(stopAfter);
  }

  carsStop(): void {
    for (const c of this.#cars.slice(1)) c.stop();
  }

  setCarProfile(lane: number, profile: Partial<CarProfile>): void {
    const car = this.#cars[lane];
    if (car) car.profile = { ...car.profile, ...profile };
  }

  setCarScript(lane: number, script: CarScript): void {
    const car = this.#cars[lane];
    if (car) car.script = script;
  }

  /** The next n event frames are logged but not sent: the app must notice the gap and RESUME. */
  dropNextEvents(n: number): void { this.#dropEvents = n; }

  /** Power loss: every link drops (BLE too), new boot, empty log. */
  powerCycle(): void {
    this.#boot(ResetCause.POWER_ON, true);
  }

  unplugUsb(): void {
    this.#usbPlugged = false;
    for (const l of [...this.#links]) if (l.kind === 'usb') this.#dropLink(l, 'USB unplugged');
  }

  plugUsb(): void {
    this.#usbPlugged = true;
    this.#usbReadyAtMs = this.scheduler.now() + 50;
  }

  /** The central walks out of range: supervision timeout. */
  bleOutOfRange(): void {
    this.#bleInRange = false;
    for (const l of [...this.#links]) if (l.kind === 'ble') this.#dropLink(l, 'BLE link lost', HCI_SUPERVISION_TIMEOUT);
  }

  bleInRange(): void { this.#bleInRange = true; }

  /** The ribbon to lane n comes off: lanes n.. drop out. */
  unplugLane(n: number): void {
    this.#chainBreak = this.#chainBreak === null ? n : Math.min(this.#chainBreak, n);
    this.#later(LEFT_DETECT_MS, () => this.#leaveFrom(n));
  }

  replugLane(n: number): void {
    if (this.#chainBreak !== null && n <= this.#chainBreak) this.#chainBreak = null;
    this.#later(LANE_BOOT_MS, () => this.#hotJoin());
  }

  /** Lane n resets (brown-out, watchdog): it and its downstream re-join in order. */
  resetLane(n: number): void {
    const slot = this.#lanes[n];
    if (!slot || !slot.present) return;
    this.#later(LEFT_DETECT_MS, () => {
      this.#leaveFrom(n);
      slot.laneSeq = 0;
      slot.lastEventUs = null;
      slot.base = null;
      slot.reference = 0;
      slot.fault = 0;
      slot.bootMs = this.scheduler.now();
      slot.resetCause = ResetCause.WATCHDOG;
      this.#later(LANE_BOOT_MS, () => this.#hotJoin());
    });
  }

  laneSyncLost(n: number): void {
    const s = this.#lanes[n];
    if (!s?.present) return;
    s.syncOk = false;
    s.syncShifts++;
    this.#laneState(n, LaneState.SYNC_LOST, 0);
  }

  laneSyncOk(n: number): void {
    const s = this.#lanes[n];
    if (!s?.present) return;
    s.syncOk = true;
    this.#laneState(n, LaneState.SYNC_OK, 0);
  }

  laneFault(n: number, status: number = Status.ERR_LANE_NACK): void {
    const s = this.#lanes[n];
    if (!s?.present) return;
    s.fault = status;
    this.#noteError(status);
    this.#laneState(n, LaneState.FAULT, status);
  }

  /** A faulted lane recovers: CONFIGURED again (0.6: the control module publishes it). */
  laneRecover(n: number): void {
    const s = this.#lanes[n];
    if (!s?.present || !s.fault) return;
    s.fault = 0;
    this.#laneState(n, LaneState.CONFIGURED, 0);
  }

  // ---- diagnostics knobs (0.6) ---------------------------------------------------------

  /** The nRF stops (false) or resumes (true) heartbeating. ABSENT 3 s after it went silent. */
  nrfAlive(alive: boolean): void {
    if (alive) {
      this.#nrf.silentSinceMs = null;
      this.scheduler.clearTimeout(this.#nrf.downTimer);
      this.#nrf.downTimer = null;
      return;
    }
    if (this.#nrf.silentSinceMs !== null) return;
    this.#nrf.silentSinceMs = this.scheduler.now();
    const hadPhone = [...this.#links].some((l) => l.kind === 'ble');
    for (const l of [...this.#links]) if (l.kind === 'ble') this.#dropLink(l, 'BLE bridge stopped', HCI_SUPERVISION_TIMEOUT, false);
    this.#nrf.downTimer = this.#later(NRF_SILENT_MS, () => {
      this.#nrf.downTimer = null;
      if (hadPhone && this.#nrf.silentSinceMs !== null) this.#bleLinkEvent(false, HCI_SUPERVISION_TIMEOUT);
    });
  }

  /** The nRF reports another link protocol MAJOR: BLE data refused. */
  nrfVersionMismatch(on: boolean): void {
    this.#nrf.mismatch = on;
    if (on) for (const l of [...this.#links]) if (l.kind === 'ble') this.#dropLink(l, 'BLE link protocol mismatch', HCI_LOCAL_HOST);
  }

  get bleBridge(): number {
    if (!this.#opts.bleModule) return BleBridgeState.ABSENT;
    if (this.#nrf.silentSinceMs !== null && this.scheduler.now() - this.#nrf.silentSinceMs >= NRF_SILENT_MS) return BleBridgeState.ABSENT;
    if (this.#nrf.mismatch) return BleBridgeState.VERSION_MISMATCH;
    return BleBridgeState.OK;
  }

  /** RSSI (dBm) the nRF reports for the connected phone. */
  setBleRssi(dbm: number): void { this.#bleRssi = Math.max(-128, Math.min(126, Math.round(dbm))); }

  /** nRF STATS and the STM's own link CRC counter (STATUS reports them saturated to 16 bits). */
  setBleStats(st: Partial<{ bleCrcErrors: number; bleUartErrors: number; bleDataDropped: number; nrfLinkCrcErrors: number }>): void {
    this.#bleStats = { ...this.#bleStats, ...st };
  }

  /** The longest super-loop pass STATUS reports. */
  setLoopMaxMs(ms: number): void { this.#loopMaxMs = Math.max(0, Math.min(0xffff, Math.round(ms))); }

  /** The settings store fails its CRC at the next boot: defaults, settingsDefaults = 1, EVT_FAULT SETTINGS. */
  corruptSettings(): void { this.#corruptAtNextBoot = true; }

  /** Flash writes fail: a LEARN ends NOT_SAVED. */
  failFlashWrites(on: boolean): void { this.#flashFails = on; }

  /** Dirty strip: the bare level drops to factor × clean (1 = clean again). */
  setLaneDirt(lane: number, factor: number): void { const hw = this.#hw[lane]; if (hw) hw.dirt = Math.max(0, factor); }

  /** Noisy sensor: noise × factor (1 = normal). */
  setLaneNoise(lane: number, factor: number): void { const hw = this.#hw[lane]; if (hw) hw.noiseFactor = Math.max(0, factor); }

  /** IR current too high for the foil: every sample clips at 65535. */
  setLaneSaturated(lane: number, on: boolean): void { const hw = this.#hw[lane]; if (hw) hw.saturated = on; }

  /** The lane's sensor stops answering (FAULT ERR_HAL, SENSOR_OK clear, raw 0); false repairs it (CONFIGURED). */
  sensorFault(lane: number, on: boolean): void {
    const hw = this.#hw[lane];
    if (!hw || hw.sensorBroken === on) return;
    hw.sensorBroken = on;
    if (on) this.laneFault(lane, Status.ERR_HAL);
    else this.laneRecover(lane);
  }

  /** The lane's bare level now (no car, no noise), for tests: what a LEARN should find. */
  laneBareLevel(lane: number): number { return Math.min(0xffff, Math.round(this.#bare(lane, this.#sysUsNow()))); }

  /** The saved reference of a lane index (control module flash), 0 = none. */
  savedReference(lane: number): number { return this.#flash.references[lane] ?? 0; }

  /** The lane's FIFO overflows: n events lost on the lane (laneSeq jumps). */
  loseLaneEvents(lane: number, n: number): void {
    const s = this.#lanes[lane];
    if (!s) return;
    s.laneSeq = (s.laneSeq + n) & 0xffff;
    s.droppedEvents += n;
  }

  /** The next START_SEQUENCE finds this lane not taking its actions. */
  failLaneSchedule(lane: number): void { this.#failSchedule.add(lane); }

  injectFault(source: number, status?: number): void {
    const st = status ?? (source === FaultSource.USB ? Status.ERR_USB_FAULT
      : source === FaultSource.EXT_POWER ? Status.ERR_POWER_FAULT : Status.ERR_LANE_POWER);
    if (source === FaultSource.USB) this.#usbFaults++;
    this.#noteError(st);
    this.#publish(EVT_FAULT, { source, reserved: 0, status: st, sysUs: this.#sysUsBig() });
  }

  dispose(): void {
    this.#disposed = true;
    this.carsStop();
    for (const t of this.#timers) this.scheduler.clearTimeout(t);
    this.#timers.clear();
    this.scheduler.clearTimeout(this.#pumpTimer);
    this.scheduler.clearTimeout(this.#railTimer);
    this.scheduler.clearTimeout(this.#settingsTimer);
    if (this.#window) this.scheduler.clearTimeout(this.#window.timer);
    if (this.#raw) this.scheduler.clearInterval(this.#raw.timer);
    this.scheduler.clearTimeout(this.#nrf.downTimer);
    for (const s of this.#seq?.timers ?? []) this.scheduler.clearTimeout(s);
    for (const l of [...this.#links]) this.#dropLink(l, 'simulator disposed');
  }

  // ---- links (SimLinkHost) --------------------------------------------------------------

  attach(kind: 'usb' | 'ble', handlers: TransportHandlers): Link {
    if (this.#disposed) throw new Error('simulator disposed');
    const now = this.scheduler.now();
    if (kind === 'usb') {
      if (!this.#usbPlugged || now < this.#usbReadyAtMs) throw new Error('no TimeTaker on USB (unplugged or re-enumerating)');
      if ([...this.#links].some((l) => l.kind === 'usb')) throw new Error('USB port busy');
    } else {
      if (!this.#opts.bleModule || !this.#flash.settings.bleEnabled || !this.#bleInRange || this.#nrf.silentSinceMs !== null
          || this.#nrf.mismatch) throw new Error('no TimeTaker advertising');
      if ([...this.#links].some((l) => l.kind === 'ble')) throw new Error('TimeTaker is connected to another central');
    }
    const link: Link = {
      kind, handlers, decoder: new FrameDecoder(PROTO.headerBytes + PROTO.maxPayload), session: null, open: true,
      kickTimer: null, budget: BLE_STREAM_BYTES_PER_S, budgetAtMs: now,
    };
    this.#links.add(link);
    if (kind === 'ble') this.#bleLinkEvent(true, 0);
    return link;
  }

  detach(link: Link, _reason: string): void {
    if (!this.#links.has(link)) return;
    this.#links.delete(link);
    link.open = false;
    this.scheduler.clearTimeout(link.kickTimer);
    if (link.kind === 'ble') this.#bleLinkEvent(false, HCI_REMOTE_USER);
  }

  receive(link: Link, bytes: Uint8Array): void {
    const copy = bytes.slice();
    const delay = link.kind === 'ble' ? this.#opts.bleLatencyMs : this.#opts.latencyMs;
    this.#later(delay, () => {
      if (!link.open) return;
      link.decoder.feed(copy, (raw) => this.#onFrame(link, raw));
    });
  }

  // ---- boot, log, pump -------------------------------------------------------------------

  #boot(cause: number, powerLoss = false): void {
    // Everything volatile goes; flash stays.
    this.#enumGen++;
    this.scheduler.clearTimeout(this.#railTimer);
    this.#rail = 'off';
    if (this.#seq) for (const t of this.#seq.timers) this.scheduler.clearTimeout(t);
    this.#seq = null;
    if (this.#raw) this.scheduler.clearInterval(this.#raw.timer);
    this.#raw = null;
    if (this.#window) this.scheduler.clearTimeout(this.#window.timer);
    this.#window = null;
    this.#failSchedule.clear();
    this.#pendingFlash.clear();
    this.#settingsDefaults = 0;
    if (this.#corruptAtNextBoot) {
      this.#corruptAtNextBoot = false;
      this.#flash = defaultFlash();
      this.#settingsDefaults = 1;
    }
    this.#laneTable = this.#flash.laneTable.map((c) => ({ ...c }));
    this.#sampling = new Array<number>(MAX_LANES + 1).fill(SamplingMode.ON);
    this.#lanes = Array.from({ length: MAX_LANES + 1 }, (_, i) => this.#newSlot(i));

    for (const l of [...this.#links]) {
      if (l.kind === 'usb' || powerLoss) this.#dropLink(l, powerLoss ? 'device lost power' : 'device rebooted', HCI_SUPERVISION_TIMEOUT, false);
      else { l.session = null; this.scheduler.clearTimeout(l.kickTimer); l.kickTimer = null; }
    }
    if (this.#bootId !== 0) this.#usbReadyAtMs = this.scheduler.now() + USB_REENUM_MS;   // not at construction

    this.#bootId = (Math.floor(this.#rng.next() * 0xfffffffe) + 1) >>> 0;
    this.#bootMs = this.scheduler.now();
    this.#log = new Array(this.#opts.logCapacity);
    this.#next = 0;
    this.#publish(EVT_BOOT, { resetCause: cause, reserved: new Uint8Array(3), bootId: this.#bootId, sysUs: 0n });
    if (this.#settingsDefaults) {
      this.#noteError(Status.ERR_SETTINGS_CORRUPT);
      this.#publish(EVT_FAULT, { source: FaultSource.SETTINGS, reserved: 0, status: Status.ERR_SETTINGS_CORRUPT, sysUs: 0n });
    }
    if ([...this.#links].some((l) => l.kind === 'ble')) this.#bleLinkEvent(true, 0);
  }

  #sysUsNow(): number {
    return Math.round((this.scheduler.now() - this.#bootMs) * 1000);
  }

  #sysUsBig(): bigint { return BigInt(this.#sysUsNow()); }

  #msAtUs(us: number): number { return this.#bootMs + us / 1000; }

  #oldest(): number { return Math.max(0, this.#next - this.#opts.logCapacity); }

  #publish(desc: StructDesc, value: object): void {
    const payload = encodeStruct(desc, { evSeq: 0, ...value });
    const msgId = BODY_ID.get(desc);
    if (msgId === undefined) throw new Error(`simulator cannot publish ${desc.name}`);
    this.#log[this.#next % this.#opts.logCapacity] = { msgId, body: payload.subarray(4) };
    this.#next++;
    this.#kick();
  }

  #later(ms: number, fn: () => void): TimerHandle {
    const t = this.scheduler.setTimeout(() => { this.#timers.delete(t); if (!this.#disposed) fn(); }, ms);
    this.#timers.add(t);
    return t;
  }

  #kick(delayMs = 0.5): void {
    if (this.#pumpTimer !== null || this.#disposed) return;
    this.#pumpTimer = this.scheduler.setTimeout(() => { this.#pumpTimer = null; this.#pump(); }, delayMs);
  }

  #pump(): void {
    for (const link of this.#links) {
      const s = link.session;
      if (!link.open || !s || s.bindState === BindState.UNBOUND) continue;
      let budget = link.kind === 'ble' ? 16 : Number.POSITIVE_INFINITY;
      while (s.cursor < this.#next && budget-- > 0) {
        if (s.cursor < this.#oldest()) s.cursor = this.#oldest();
        const seq = s.cursor++;
        const entry = this.#log[seq % this.#opts.logCapacity];
        if (!entry) continue;
        if (this.#dropEvents > 0) { this.#dropEvents--; continue; }
        const head = new Uint8Array(PROTO.headerBytes + 4);
        head.set([entry.msgId, HeaderFlag.EVENT, 0, 0]);
        new DataView(head.buffer).setUint32(PROTO.headerBytes, seq, true);
        this.#send(link, frameEncode(head, entry.body));
      }
      if (s.cursor < this.#next) this.#kick(7.5);
    }
  }

  #send(link: Link, bytes: Uint8Array): void {
    const n = link.kind === 'ble' ? 244 : this.#opts.chunkBytes;
    const delay = link.kind === 'ble' ? this.#opts.bleLatencyMs : this.#opts.latencyMs;
    for (let i = 0; i < bytes.length; i += n) {
      const chunk = bytes.slice(i, i + n);
      this.#later(delay, () => { if (link.open && this.#links.has(link)) link.handlers.onBytes(chunk); });
    }
  }

  #dropLink(link: Link, reason: string, hciReason = HCI_LOCAL_HOST, publish = true): void {
    if (!this.#links.has(link)) return;
    this.#links.delete(link);
    link.open = false;
    this.scheduler.clearTimeout(link.kickTimer);
    if (link.kind === 'ble' && publish) this.#bleLinkEvent(false, hciReason);
    link.handlers.onClose(reason);
  }

  #bleLinkEvent(up: boolean, reason: number): void {
    this.#publish(EVT_BLE_LINK, {
      state: up ? BleLinkState.UP : BleLinkState.DOWN, phy: up ? 2 : 0, attMtu: up ? 247 : 0,
      intervalUnits: up ? 24 : 0, reason, reserved: 0, sysUs: this.#sysUsBig(),
    });
  }

  #stream(desc: StructDesc, msgId: number, value: object): void {
    let payload: Uint8Array;
    try { payload = encodeStruct(desc, value); } catch { return; }
    const frame = encodePacket({ msgId, flags: HeaderFlag.STREAM, tag: 0 }, payload);
    const now = this.scheduler.now();
    for (const link of this.#links) {
      if (!link.open || !link.session || link.session.bindState === BindState.UNBOUND) continue;
      if (link.kind === 'ble') {
        link.budget = Math.min(BLE_STREAM_BYTES_PER_S, link.budget + ((now - link.budgetAtMs) / 1000) * BLE_STREAM_BYTES_PER_S);
        link.budgetAtMs = now;
        if (link.budget < frame.length) continue;   // dropped: best effort
        link.budget -= frame.length;
      }
      this.#send(link, frame);
    }
  }

  #dbg(level: number, text: string): void {
    this.#stream(DBG_LOG, MsgId.DBG_LOG, { level, reserved: new Uint8Array(3), sysUs: this.#sysUsBig(), text: text.slice(0, 200) });
  }

  // ---- requests --------------------------------------------------------------------------

  #respond(link: Link, msgId: number, tag: number, status: number, desc?: StructDesc, body?: object): void {
    if (!link.open) return;
    let payload: Uint8Array;
    if (status !== Status.OK || !desc) {
      payload = new Uint8Array(2);
      new DataView(payload.buffer).setInt16(0, status, true);
    } else {
      payload = encodeStruct(desc, { ...body, status: Status.OK });
    }
    this.#send(link, encodePacket({ msgId, flags: HeaderFlag.RESPONSE, tag }, payload));
  }

  #onFrame(link: Link, raw: Uint8Array): void {
    let pkt: ReturnType<typeof decodePacket>;
    try { pkt = decodePacket(raw); } catch { return; }
    const { header: h, payload } = pkt;
    if (h.flags !== 0) return;                       // only requests come this way

    const reply = (status: number, desc?: StructDesc, body?: object): void => this.#respond(link, h.msgId, h.tag, status, desc, body);
    const parse = (desc: StructDesc): Record<string, unknown> | null => {
      const hasTail = desc.fields.length > 0 && desc.fields[desc.fields.length - 1]!.kind === 'tail';
      if (hasTail ? payload.length < desc.size : payload.length !== desc.size) { reply(Status.ERR_BAD_LENGTH); return null; }
      try { return decodeStruct(desc, payload); } catch { reply(Status.ERR_BAD_LENGTH); return null; }
    };
    const empty = (): boolean => {
      if (payload.length !== 0) { reply(Status.ERR_BAD_LENGTH); return false; }
      return true;
    };

    if (h.msgId === MsgId.HELLO) {
      const req = parse(HELLO_REQ);
      if (req) this.#hello(link, req, reply);
      return;
    }
    const s = link.session;
    if (!s) { reply(Status.ERR_NO_SESSION); return; }
    if (h.msgId === MsgId.PING) {
      const req = parse(PING_REQ);
      if (req) reply(Status.OK, PING_RSP, { hostMs: req['hostMs'], sysUs: this.#sysUsBig() });
      return;
    }
    if (s.bindState === BindState.UNBOUND) { reply(Status.ERR_NOT_BOUND); return; }

    if (h.msgId === MsgId.RESUME) {
      const req = parse(RESUME_REQ);
      if (!req) return;
      const from = req['fromSeq'] as number;
      if (req['bootId'] !== this.#bootId) reply(Status.ERR_BOOT_MISMATCH);
      else if (from < this.#oldest()) reply(Status.ERR_LOG_GAP);
      else if (from > this.#next) reply(Status.ERR_INVALID_ARG);
      else {
        s.cursor = from;
        reply(Status.OK, RESUME_RSP, { logOldestSeq: this.#oldest(), logNextSeq: this.#next });
        this.#kick();
      }
      return;
    }
    if (h.msgId === MsgId.NODE_LIST) {
      if (empty()) reply(Status.OK, NODE_LIST_RSP, { nodes: this.#nodes() });
      return;
    }

    this.#appRequest(link, h.msgId, parse, empty, reply);
  }

  #hello(link: Link, req: Record<string, unknown>, reply: (st: number, d?: StructDesc, b?: object) => void): void {
    if (req['protoMajor'] !== PROTO.major) {
      link.session = null;
      reply(Status.ERR_PROTO_MISMATCH);
      return;
    }
    const clientId = (req['clientId'] as Uint8Array).slice();
    const clientName = req['clientName'] as string;
    const clientKind = req['clientKind'] as number;
    let bindState: number;
    if (link.kind === 'usb') {
      bindState = BindState.TRUSTED;
      if (clientKind === ClientKind.WEBAPP && !isZero(clientId)) this.#bindAddOrTouch(clientId, clientName);
    } else if (this.#bindIndex(clientId) >= 0) {
      bindState = BindState.BOUND;
      this.#bindAddOrTouch(clientId, clientName);
    } else if (this.#window && !isZero(clientId)) {
      bindState = BindState.BOUND;
      this.#bindAddOrTouch(clientId, clientName);
    } else {
      bindState = BindState.UNBOUND;
    }
    link.session = { clientId, clientName, clientKind, bindState, cursor: this.#next };
    if (bindState === BindState.UNBOUND) this.#armKick(link);
    else { this.scheduler.clearTimeout(link.kickTimer); link.kickTimer = null; }

    const others = [...this.#links].filter((l) => l !== link && l.open && l.session).length;
    reply(Status.OK, HELLO_RSP, {
      protoMajor: PROTO.major, protoMinor: PROTO.minor, transport: link.kind === 'usb' ? HpTransport.USB : HpTransport.BLE,
      bindState, bootId: this.#bootId, sysUs: this.#sysUsBig(), logOldestSeq: this.#oldest(), logNextSeq: this.#next,
      otherSessions: others, boardType: BoardType.CONTROL,
      reserved: new Uint8Array(2),
    });
    this.#kick();
  }

  #appRequest(link: Link, msgId: number, parse: (d: StructDesc) => Record<string, unknown> | null, empty: () => boolean,
              reply: (st: number, d?: StructDesc, b?: object) => void): void {
    switch (msgId) {
      case MsgId.BIND_LIST: {
        const req = parse(BIND_LIST_REQ);
        if (!req) return;
        const start = req['start'] as number;
        const total = this.#flash.binds.length;
        if (start > total) { reply(Status.ERR_INVALID_ARG); return; }
        const entries: BindEntry[] = this.#flash.binds.slice(start, start + BIND_PAGE)
          .map((b) => ({ clientId: b.clientId, clientName: b.clientName, lastUsed: b.lastUsed }));
        reply(Status.OK, BIND_LIST_RSP, { total, start, windowS: this.#windowS(), entries });
        return;
      }
      case MsgId.BIND_REMOVE: {
        const req = parse(BIND_REMOVE_REQ);
        if (!req) return;
        const id = req['clientId'] as Uint8Array;
        if (isZero(id)) this.#clearBinds(false);
        else this.#bindRemove(id);
        reply(Status.OK, BIND_REMOVE_RSP, {});
        return;
      }
      case MsgId.BIND_ALLOW: {
        const req = parse(BIND_ALLOW_REQ);
        if (!req) return;
        const w = req['windowS'] as number;
        if (w === 0) this.#closeWindow(); else this.#openWindow(Math.min(w, BIND_WINDOW_MAX_S));
        reply(Status.OK, BIND_ALLOW_RSP, {});
        return;
      }
      case MsgId.REBOOT: {
        const req = parse(REBOOT_REQ);
        if (!req) return;
        const target = req['target'] as number;
        if (target === RebootTarget.BOOTLOADER) { reply(Status.ERR_UNSUPPORTED); return; }   // no bootloader in v1
        if (target !== RebootTarget.APP) { reply(Status.ERR_INVALID_ARG); return; }
        reply(Status.OK, REBOOT_RSP, {});
        this.#later(REBOOT_DELAY_MS, () => this.#boot(ResetCause.SOFTWARE));
        return;
      }
      case MsgId.STATUS: {
        if (!empty()) return;
        // nRF STATS come every second: RSSI and the nRF counters are known from the first one on.
        const stats = this.bleBridge === BleBridgeState.OK && this.scheduler.now() - this.#bootMs >= 1000;
        reply(Status.OK, STATUS_RSP, {
          lastError: this.#lastError, loopMaxMs: this.#loopMaxMs, errorCount: this.#errorCount,
          uptimeMs: Math.floor(this.#sysUsNow() / 1000) >>> 0, lanesPresent: this.#presentCount(),
          lanesPowered: this.#rail === 'on' ? 1 : 0,
          bleLink: [...this.#links].some((l) => l.kind === 'ble') ? BleLinkState.UP : BleLinkState.DOWN,
          seqRunning: this.#seq ? 1 : 0, usbFaults: this.#usbFaults, settingsWrites: this.#settingsWrites,
          logOldestSeq: this.#oldest(), logNextSeq: this.#next,
          bleBridge: this.bleBridge,
          bleRssi: stats && [...this.#links].some((l) => l.kind === 'ble') ? this.#bleRssi : RSSI_UNKNOWN,
          bleCrcErrors: stats ? sat16(this.#bleStats.bleCrcErrors) : 0, bleUartErrors: stats ? sat16(this.#bleStats.bleUartErrors) : 0,
          bleDataDropped: stats ? sat16(this.#bleStats.bleDataDropped) : 0, nrfLinkCrcErrors: sat16(this.#bleStats.nrfLinkCrcErrors),
          settingsDefaults: this.#settingsDefaults, reserved: 0,
        });
        return;
      }
      case MsgId.SETTINGS_GET: {
        if (!empty()) return;
        reply(Status.OK, SETTINGS_GET_RSP, { settings: { ...this.#flash.settings, reserved: new Uint8Array(3) } });
        return;
      }
      case MsgId.SETTINGS_SET: {
        const req = parse(SETTINGS_SET_REQ);
        if (!req) return;
        const st = req['settings'] as { deviceName: string; bleEnabled: number };
        if (st.bleEnabled > 1) { reply(Status.ERR_INVALID_ARG); return; }
        this.#flash.settings = { deviceName: st.deviceName, bleEnabled: st.bleEnabled };
        this.#markSettingsDirty();
        reply(Status.OK, SETTINGS_SET_RSP, {});
        if (!st.bleEnabled) for (const l of [...this.#links]) if (l.kind === 'ble' && l !== link) this.#dropLink(l, 'BLE disabled in the settings');
        if (!st.bleEnabled && link.kind === 'ble') this.#later(20, () => this.#dropLink(link, 'BLE disabled in the settings'));
        return;
      }
      case MsgId.LANE_SCAN: {
        if (!empty()) return;
        if (this.#rail !== 'on') { reply(Status.ERR_INVALID_STATE); return; }
        reply(Status.OK, LANE_SCAN_RSP, {});
        this.#enumGen++;
        this.#leaveFrom(1);
        this.#startEnumeration(LANE_JOIN_STEP_MS * 2);
        return;
      }
      case MsgId.LANE_POWER: {
        const req = parse(LANE_POWER_REQ);
        if (!req) return;
        const on = req['on'] as number;
        if (on > 1) { reply(Status.ERR_INVALID_ARG); return; }
        this.#setRail(on === 1);
        reply(Status.OK, LANE_POWER_RSP, { powered: this.#rail === 'on' ? 1 : 0 });
        return;
      }
      case MsgId.LANE_CONFIG_GET: {
        const req = parse(LANE_CONFIG_GET_REQ);
        if (!req) return;
        const lane = req['lane'] as number;
        if (lane < 1 || lane > MAX_LANES) { reply(Status.ERR_INVALID_ARG); return; }
        reply(Status.OK, LANE_CONFIG_GET_RSP, { lane, reserved: new Uint8Array(3), config: this.#laneTable[lane] });
        return;
      }
      case MsgId.LANE_CONFIG_SET: {
        const req = parse(LANE_CONFIG_SET_REQ);
        if (!req) return;
        const mask = req['laneMask'] as number;
        const cfg = req['config'] as LaneConfig;
        if (!this.#maskOk(mask) || (req['save'] as number) > 1 || cfg.samplePeriodUs === 0 || cfg.rawDecimation === 0
            || cfg.thrDropPermille > 1000 || cfg.hystPermille > 1000) { reply(Status.ERR_INVALID_ARG); return; }
        for (let lane = 1; lane <= MAX_LANES; lane++) {
          if (!(mask & (1 << (lane - 1)))) continue;
          this.#laneTable[lane] = { ...cfg };
          if (req['save'] === 1) this.#flash.laneTable[lane] = { ...cfg };
        }
        if (req['save'] === 1) this.#markSettingsDirty();
        reply(Status.OK, LANE_CONFIG_SET_RSP, {});
        return;
      }
      case MsgId.LANE_SAMPLING: {
        const req = parse(LANE_SAMPLING_REQ);
        if (!req) return;
        const mask = req['laneMask'] as number;
        const mode = req['mode'] as number;
        if (!this.#maskOk(mask) || mode > SamplingMode.RAW) { reply(Status.ERR_INVALID_ARG); return; }
        for (let lane = 1; lane <= MAX_LANES; lane++) {
          if (!(mask & (1 << (lane - 1)))) continue;
          if (this.#raw?.lane === lane) this.#raw.prevMode = mode;
          else this.#sampling[lane] = mode;
        }
        reply(Status.OK, LANE_SAMPLING_RSP, {});
        return;
      }
      case MsgId.LANE_LED: {
        const req = parse(LANE_LED_REQ);
        if (!req) return;
        const mask = req['laneMask'] as number;
        const led: LedState = {
          mode: req['mode'] as number, level: req['level'] as number, blinkPeriodMs: req['blinkPeriodMs'] as number,
          blinkOnPermille: req['blinkOnPermille'] as number, blinkCount: req['blinkCount'] as number,
        };
        if (this.#seq) { reply(Status.ERR_SEQ_RUNNING); return; }
        if (!this.#maskOk(mask) || led.mode > LedMode.BLINK || led.level > 1000 || led.blinkOnPermille > 1000
            || (led.mode === LedMode.BLINK && led.blinkPeriodMs === 0)) { reply(Status.ERR_INVALID_ARG); return; }
        const lanes = this.#presentIn(mask);
        if (lanes.length === 0) { reply(Status.ERR_LANE_ABSENT); return; }
        for (const l of lanes) this.#lanes[l]!.led = led;
        reply(Status.OK, LANE_LED_RSP, {});
        return;
      }
      case MsgId.LANE_STATUS: {
        const req = parse(LANE_STATUS_REQ);
        if (!req) return;
        const lane = req['lane'] as number;
        if (lane < 1 || lane > MAX_LANES) { reply(Status.ERR_INVALID_ARG); return; }
        const s = this.#lanes[lane]!;
        if (!s.present) { reply(Status.ERR_LANE_ABSENT); return; }
        reply(Status.OK, LANE_STATUS_RSP, this.#laneStatus(lane));
        return;
      }
      case MsgId.LANE_RAW_STREAM: {
        const req = parse(LANE_RAW_STREAM_REQ);
        if (!req) return;
        const lane = req['lane'] as number;
        const dec = req['decimation'] as number;
        if (lane > MAX_LANES || (lane !== 0 && dec === 0)) { reply(Status.ERR_INVALID_ARG); return; }
        if (lane !== 0 && !this.#lanes[lane]!.present) { reply(Status.ERR_LANE_ABSENT); return; }
        this.#stopRaw();
        if (lane !== 0) this.#startRaw(lane, dec);
        reply(Status.OK, LANE_RAW_STREAM_RSP, {});
        return;
      }
      case MsgId.LANE_CALIBRATE: {
        const req = parse(LANE_CALIBRATE_REQ);
        if (!req) return;
        const mask = req['laneMask'] as number;
        const action = req['action'] as number;
        if (!this.#maskOk(mask) || action > CalibrateAction.FORGET) { reply(Status.ERR_INVALID_ARG); return; }
        if (this.#seq) { reply(Status.ERR_SEQ_RUNNING); return; }
        if (action === CalibrateAction.LEARN) {
          const lanes = this.#presentIn(mask).filter((l) => this.#lanes[l]!.configured);
          if (lanes.length === 0) { reply(Status.ERR_LANE_ABSENT); return; }
          reply(Status.OK, LANE_CALIBRATE_RSP, {});
          for (const l of lanes) this.#learn(l, LEARN_MS, true);
          return;
        }
        reply(Status.OK, LANE_CALIBRATE_RSP, {});
        for (let l = 1; l <= MAX_LANES; l++) {
          if (!(mask & (1 << (l - 1)))) continue;
          this.#pendingFlash.set(l, { action: CalibrateAction.FORGET, reference: 0 });
          const s = this.#lanes[l]!;
          if (s.present && s.configured) this.#learn(l, this.#learnBootMs(l), false);
        }
        this.#markSettingsDirty();
        return;
      }
      case MsgId.START_SEQUENCE: {
        const req = parse(START_SEQUENCE_REQ);
        if (req) this.#startSequence(req, reply);
        return;
      }
      case MsgId.ABORT_SEQUENCE: {
        if (!empty()) return;
        if (!this.#seq) { reply(Status.ERR_INVALID_STATE); return; }
        this.#abortSequence(SeqAbortReason.HOST);
        reply(Status.OK, ABORT_SEQUENCE_RSP, {});
        return;
      }
      default:
        reply(Status.ERR_UNSUPPORTED);
    }
  }

  // ---- binding -------------------------------------------------------------------------------

  #bindIndex(id: Uint8Array): number {
    return this.#flash.binds.findIndex((b) => bytesEqual(b.clientId, id));
  }

  #bindEvent(change: number, id: Uint8Array | null): void {
    this.#publish(EVT_BIND_CHANGED, {
      change, count: this.#flash.binds.length, windowS: this.#windowS(),
      clientIdPrefix: id ? id.slice(0, 8) : new Uint8Array(8),
    });
  }

  #bindAddOrTouch(id: Uint8Array, name: string): void {
    const i = this.#bindIndex(id);
    if (i >= 0) {
      this.#flash.binds[i]!.lastUsed = ++this.#lru;
      this.#flash.binds[i]!.clientName = name;
      return;
    }
    if (this.#flash.binds.length >= BIND_MAX) {
      let victim = 0;
      this.#flash.binds.forEach((b, j) => { if (b.lastUsed < this.#flash.binds[victim]!.lastUsed) victim = j; });
      const [gone] = this.#flash.binds.splice(victim, 1);
      this.#bindEvent(BindChange.REMOVED, gone!.clientId);
      for (const l of this.#links) if (l.kind === 'ble' && l.session && bytesEqual(l.session.clientId, gone!.clientId)) this.#unbind(l);
    }
    this.#flash.binds.push({ clientId: id.slice(), clientName: name, lastUsed: ++this.#lru });
    this.#markSettingsDirty();
    this.#bindEvent(BindChange.ADDED, id);
  }

  #bindRemove(id: Uint8Array): void {
    const i = this.#bindIndex(id);
    if (i < 0) return;
    this.#flash.binds.splice(i, 1);
    this.#markSettingsDirty();
    this.#bindEvent(BindChange.REMOVED, id);
    for (const l of this.#links) if (l.kind === 'ble' && l.session && bytesEqual(l.session.clientId, id)) this.#unbind(l);
  }

  #clearBinds(closeWindow: boolean): void {
    this.#flash.binds = [];
    this.#markSettingsDirty();
    this.#bindEvent(BindChange.CLEARED, null);
    if (closeWindow) this.#closeWindow();
    for (const l of this.#links) if (l.kind === 'ble' && l.session) this.#unbind(l);
  }

  #unbind(link: Link): void {
    if (!link.session || link.session.bindState === BindState.UNBOUND) return;
    link.session.bindState = BindState.UNBOUND;
    this.#armKick(link);
  }

  #armKick(link: Link): void {
    if (link.kickTimer !== null) return;
    link.kickTimer = this.#later(this.#opts.unboundKickMs, () => {
      link.kickTimer = null;
      if (!link.open || link.session?.bindState !== BindState.UNBOUND) return;
      this.#bindEvent(BindChange.REJECTED, link.session.clientId);
      this.#dropLink(link, 'kicked: not bound within 60 s', HCI_LOCAL_HOST);
    });
  }

  #windowS(): number {
    return this.#window ? Math.max(0, Math.ceil((this.#window.untilMs - this.scheduler.now()) / 1000)) : 0;
  }

  #openWindow(seconds: number): void {
    if (this.#window) this.scheduler.clearTimeout(this.#window.timer);
    const timer = this.#later(seconds * 1000, () => { this.#window = null; this.#bindEvent(BindChange.WINDOW_CLOSED, null); });
    this.#window = { untilMs: this.scheduler.now() + seconds * 1000, timer };
    this.#bindEvent(BindChange.WINDOW_OPENED, null);
    for (const l of this.#links) {
      const s = l.session;
      if (l.kind === 'ble' && s && s.bindState === BindState.UNBOUND && !isZero(s.clientId)) {
        s.bindState = BindState.BOUND;
        this.scheduler.clearTimeout(l.kickTimer);
        l.kickTimer = null;
        this.#bindAddOrTouch(s.clientId, s.clientName);
        this.#kick();
      }
    }
  }

  #closeWindow(): void {
    if (!this.#window) return;
    this.scheduler.clearTimeout(this.#window.timer);
    this.#window = null;
    this.#bindEvent(BindChange.WINDOW_CLOSED, null);
  }

  #markSettingsDirty(): void {
    this.#settingsDirty = true;
    if (this.#settingsTimer !== null) return;
    const due = Math.max(0, this.#lastSettingsWriteMs + SETTINGS_MIN_INTERVAL_MS - this.scheduler.now());
    this.#settingsTimer = this.scheduler.setTimeout(() => this.#writeSettings(), due);
  }

  #writeSettings(): void {
    this.#settingsTimer = null;
    if (!this.#settingsDirty || this.#disposed) return;
    if (this.#seq) { this.#settingsTimer = this.scheduler.setTimeout(() => this.#writeSettings(), 500); return; }
    this.#settingsDirty = false;
    this.#lastSettingsWriteMs = this.scheduler.now();
    const pending = [...this.#pendingFlash].sort((a, b) => a[0] - b[0]);
    this.#pendingFlash.clear();
    if (!this.#flashFails) this.#settingsWrites++;
    for (const [lane, p] of pending) {
      if (this.#flashFails) { this.#calibrated(lane, CalibrateResult.NOT_SAVED, p.reference); continue; }
      this.#flash.references[lane] = p.reference;
      this.#calibrated(lane, p.action === CalibrateAction.LEARN ? CalibrateResult.SAVED : CalibrateResult.FORGOTTEN, p.reference);
    }
  }

  #noteError(status: number): void {
    this.#lastError = status;
    this.#errorCount++;
  }

  // ---- lanes ---------------------------------------------------------------------------------

  #newSlot(index: number): LaneSlot {
    return {
      index, present: false, configured: false, laneSeq: 0, lastEventUs: null, bootMs: this.scheduler.now(),
      resetCause: ResetCause.POWER_ON, led: { ...LED_OFF }, syncOk: true, fault: 0,
      base: null, reference: 0, learning: null, learnGen: 0, winFromUs: 0,
      passes: [], droppedEvents: 0, syncShifts: 0, badWrites: 0, lateActions: 0,
    };
  }

  #maskOk(mask: number): boolean { return mask !== 0 && mask <= 0x3f; }

  #presentIn(mask: number): number[] {
    const out: number[] = [];
    for (let l = 1; l <= MAX_LANES; l++) if (mask & (1 << (l - 1)) && this.#lanes[l]!.present) out.push(l);
    return out;
  }

  #presentCount(): number {
    let n = 0;
    while (n < MAX_LANES && this.#lanes[n + 1]!.present) n++;
    return n;
  }

  #reachable(n: number): boolean {
    return this.#rail === 'on' && n <= this.#chainLength && (this.#chainBreak === null || n < this.#chainBreak);
  }

  #effectiveMode(lane: number): number {
    if (this.#raw?.lane === lane) return SamplingMode.RAW;
    return this.#sampling[lane] ?? SamplingMode.OFF;
  }

  #setRail(on: boolean): void {
    if (!on) {
      if (this.#rail === 'off') return;
      this.#enumGen++;
      this.scheduler.clearTimeout(this.#railTimer);
      if (this.#seq) this.#abortSequence(SeqAbortReason.POWER);
      this.#stopRaw();
      for (let l = 1; l <= MAX_LANES; l++) {
        const s = this.#lanes[l]!;
        if (s.present) { s.present = false; s.configured = false; this.#laneGone(l); this.#laneState(l, LaneState.LEFT, 0); }
        s.led = { ...LED_OFF };
      }
      this.#rail = 'off';
      return;
    }
    if (this.#rail !== 'off') return;
    this.#rail = 'settling';
    this.#railTimer = this.scheduler.setTimeout(() => {
      this.#railTimer = null;
      this.#rail = 'on';
      for (let l = 1; l <= MAX_LANES; l++) {
        const s = this.#lanes[l]!;
        s.laneSeq = 0; s.lastEventUs = null; s.bootMs = this.scheduler.now(); s.resetCause = ResetCause.POWER_ON;
        s.syncOk = true; s.fault = 0; s.base = null; s.reference = 0;
      }
      this.#dbg(LogLevel.INFO, 'LANE rail on, SYNC on');
      this.#startEnumeration(LANE_BOOT_MS);
    }, LANE_SETTLE_MS);
  }

  #startEnumeration(delayMs: number): void {
    const gen = ++this.#enumGen;
    let first = 1;
    while (first <= MAX_LANES && this.#lanes[first]!.present) first++;
    this.#later(delayMs, () => this.#tryJoin(first, gen));
  }

  #hotJoin(): void {
    if (this.#rail !== 'on') return;
    this.#startEnumeration(0);
  }

  #tryJoin(n: number, gen: number): void {
    if (gen !== this.#enumGen || n > MAX_LANES || !this.#reachable(n)) return;
    const s = this.#lanes[n]!;
    if (s.present) { this.#later(LANE_JOIN_STEP_MS, () => this.#tryJoin(n + 1, gen)); return; }
    s.present = true;
    s.configured = false;
    this.#laneState(n, LaneState.JOINED, 0);
    this.#dbg(LogLevel.INFO, `LANE ${n} JOIN addr=0x${(0x10 + n).toString(16)}`);
    this.#later(CONFIG_PUSH_MS, () => {
      if (gen !== this.#enumGen || !s.present) return;
      s.configured = true;
      this.#seedReference(n);
      this.#laneState(n, LaneState.CONFIGURED, 0);
      if (this.#hw[n]!.sensorBroken) this.laneFault(n, Status.ERR_HAL);
      this.#later(LANE_JOIN_STEP_MS, () => this.#tryJoin(n + 1, gen));
    });
  }

  #leaveFrom(n: number): void {
    let any = false;
    for (let l = n; l <= MAX_LANES; l++) {
      const s = this.#lanes[l]!;
      if (!s.present) continue;
      s.present = false;
      s.configured = false;
      any = true;
      if (this.#raw?.lane === l) this.#stopRaw();
      this.#laneGone(l);
      this.#laneState(l, LaneState.LEFT, 0);
      if (this.#seq?.lanes.includes(l)) this.#abortSequence(SeqAbortReason.LANE_LEFT);
    }
    if (any) this.#dbg(LogLevel.WARN, `LANE ${n}+ LEFT`);
  }

  #laneState(lane: number, state: number, status: number): void {
    this.#publish(EVT_LANE_STATE, { lane, state, status, sysUs: this.#sysUsBig() });
  }

  // ---- crossings -------------------------------------------------------------------------------

  #onCross(lane: number, atMs: number, kind: CrossingKind, d: CrossingDetail): void {
    const s = this.#lanes[lane]!;
    const tUs = Math.round((atMs - this.#bootMs) * 1000);
    const detected = this.#rail === 'on' && s.present && s.configured
      && this.#effectiveMode(lane) !== SamplingMode.OFF;
    this.#truth.push({ lane, bootId: this.#bootId, tEntryUs: tUs, kind, detected });
    if (!detected) return;

    const cfg = this.#laneTable[lane]!;
    let flags = 0;
    if (cfg.holdoffUs > 0 && s.lastEventUs !== null && tUs - s.lastEventUs < cfg.holdoffUs) flags |= LaneEventFlag.IN_HOLDOFF;
    if (d.merged) flags |= LaneEventFlag.MERGED;
    if (!s.syncOk) flags |= LaneEventFlag.EXTRAPOLATED;
    // lane_protocol.h: the window runs from the last CROSSING not itself flagged IN_HOLDOFF.
    if (kind !== 'blocked' && !(flags & LaneEventFlag.IN_HOLDOFF)) s.lastEventUs = tUs;
    const dwellUs = kind === 'blocked' ? cfg.blockTimeoutUs : Math.round(d.dwellMs * 1000);
    s.passes.push({ startUs: tUs, endUs: tUs + (kind === 'blocked' ? Math.round(d.blockMs * 1000) : dwellUs), depth: d.depth });
    if (s.passes.length > 16) s.passes.shift();
    const boot = this.#bootId;
    const poll = (): number => this.#rng.range(0, POLL_MS);

    const emit = (atMsPub: number, type: number, tEntryUs: number, dwell10us: number, fl: number): void => {
      this.#later(Math.max(0, atMsPub - this.scheduler.now()), () => {
        if (this.#bootId !== boot || !s.present) return;
        const seq = s.laneSeq;
        s.laneSeq = (s.laneSeq + 1) & 0xffff;
        this.#publish(EVT_CROSSING, {
          lane, type, flags: fl, reserved: 0, tEntryUs: BigInt(tEntryUs), laneSeq: seq,
          dwell10us: Math.min(0xffff, Math.round(dwell10us)), depth: d.depth, reserved2: 0,
        });
      });
    };

    if (kind === 'blocked') {
      emit(atMs + cfg.blockTimeoutUs / 1000 + poll(), LaneEventType.BLOCKED, tUs, cfg.blockTimeoutUs / 10, flags);
      const clearUs = tUs + Math.round(d.blockMs * 1000);
      // lane_protocol.h: CLEARED carries its BLOCKED event's tEntryUs; dwell is the whole blocked time.
      emit(this.#msAtUs(clearUs) + cfg.mergeGapUs / 1000 + poll(), LaneEventType.CLEARED, tUs, d.blockMs * 100, 0);
      return;
    }
    emit(atMs + d.dwellMs + cfg.mergeGapUs / 1000 + poll(), LaneEventType.CROSSING, tUs, dwellUs / 10, flags);
  }

  // ---- raw stream -------------------------------------------------------------------------------

  /** The bare-foil level (no car) at sysUs tUs: clean level × dirt × ambient drift; saturated: clipped above 65535. */
  #bare(lane: number, tUs: number): number {
    const hw = this.#hw[lane]!;
    if (hw.saturated) return 90000;
    const absMs = this.#bootMs + tUs / 1000;
    const drift = 1 + DRIFT_AMPLITUDE * Math.sin(2 * Math.PI * absMs / DRIFT_PERIOD_MS + lane * 1.1);
    return hw.bare0 * hw.dirt * drift;
  }

  /** The PS sample the lane takes at sysUs tUs (a pure function of seed, lane and time). */
  #sample(lane: number, tUs: number): number {
    const hw = this.#hw[lane]!;
    if (hw.sensorBroken) return 0;
    const s = this.#lanes[lane]!;
    const periodUs = this.#laneTable[lane]!.samplePeriodUs;
    const idx = Math.floor((this.#bootMs * 1000 + tUs) / periodUs);
    let v = this.#bare(lane, tUs) + gauss(this.#signalSeed, lane, idx) * hw.noiseSd * hw.noiseFactor;
    for (const p of s.passes) if (tUs >= p.startUs && tUs <= p.endUs) v -= p.depth * DIP_PER_DEPTH * hw.dirt;
    return Math.max(0, Math.min(0xffff, Math.round(v)));
  }

  /** Sample times (sysUs) of the lane in [fromUs, toUs]. */
  #sampleTimes(lane: number, fromUs: number, toUs: number, maxEval = Number.POSITIVE_INFINITY): number[] {
    const periodUs = this.#laneTable[lane]!.samplePeriodUs;
    const first = Math.ceil(fromUs / periodUs);
    const last = Math.floor(toUs / periodUs);
    if (last < first) return [];
    const stride = Math.max(1, Math.ceil((last - first + 1) / maxEval));
    const out: number[] = [];
    for (let i = first; i <= last; i += stride) out.push(i * periodUs);
    return out;
  }

  /** The tracked baseline now (tracks the bare level with BASELINE_TAU_US; frozen while the sensor is broken). */
  #baseline(lane: number, nowUs: number): number {
    const s = this.#lanes[lane]!;
    if (!s.base) return 0;
    const dt = nowUs - s.base.atUs;
    if (dt > 0 && !this.#hw[lane]!.sensorBroken) {
      const target = Math.min(0xffff, this.#bare(lane, nowUs));
      s.base.val += (target - s.base.val) * (1 - Math.exp(-dt / BASELINE_TAU_US));
      s.base.atUs = nowUs;
    }
    return Math.max(0, Math.min(0xffff, Math.round(s.base.val)));
  }

  #learnBootMs(lane: number): number {
    return LEARN_BOOT_SAMPLES * this.#laneTable[lane]!.samplePeriodUs / 1000;
  }

  /** Starts learning the bare level for ms; report = publish EVT_LANE_CALIBRATED (LEARN) when done. */
  #learn(lane: number, ms: number, report: boolean): void {
    const s = this.#lanes[lane]!;
    const gen = ++s.learnGen;
    const fromUs = this.#sysUsNow();
    s.learning = { gen, fromUs, untilUs: fromUs + ms * 1000, report };
    this.#later(ms, () => {
      if (s.learning?.gen !== gen || !s.present) return;
      s.learning = null;
      const hw = this.#hw[lane]!;
      if (hw.sensorBroken) {
        if (report) this.#calibrated(lane, CalibrateResult.FAILED, 0);
        return;
      }
      const nowUs = this.#sysUsNow();
      const times = this.#sampleTimes(lane, fromUs, nowUs);
      const level = times.length ? times.reduce((a, t) => a + this.#sample(lane, t), 0) / times.length : this.#sample(lane, nowUs);
      const ref = Math.max(1, Math.min(0xffff, Math.round(level)));
      s.base = { val: ref, atUs: nowUs };
      s.reference = ref;               // in use at once: LANE_STATUS shows it before the flash write
      if (!report) return;
      s.winFromUs = nowUs + 1;          // the calibration's own SIGNAL read resets rawMin/rawMax
      this.#pendingFlash.set(lane, { action: CalibrateAction.LEARN, reference: ref });
      this.#markSettingsDirty();
    });
  }

  #laneNoise(lane: number): number {
    const hw = this.#hw[lane]!;
    if (hw.saturated || hw.sensorBroken) return 0;
    return Math.round(hw.noiseSd * hw.noiseFactor);
  }

  #calibrated(lane: number, result: number, reference: number): void {
    this.#publish(EVT_LANE_CALIBRATED, {
      lane, result, reference, noise: this.#laneNoise(lane), reserved: 0, sysUs: this.#sysUsBig(),
    });
  }

  /** At CONFIGURED: SET_REFERENCE with the saved value, or learn as at boot. */
  #seedReference(lane: number): void {
    const s = this.#lanes[lane]!;
    const saved = this.#flash.references[lane] ?? 0;
    s.winFromUs = this.#sysUsNow();
    if (saved) {
      s.learning = null;
      s.learnGen++;
      s.base = { val: saved, atUs: this.#sysUsNow() };
      s.reference = saved;
    } else if (!s.base && !s.learning) {
      this.#learn(lane, this.#learnBootMs(lane), false);
    }
  }

  /** A lane dropped off the bus: a LEARN in progress fails. */
  #laneGone(lane: number): void {
    const s = this.#lanes[lane]!;
    const pending = s.learning;
    s.learning = null;
    s.learnGen++;
    if (pending?.report) this.#calibrated(lane, CalibrateResult.FAILED, 0);
  }

  #laneStatus(lane: number): Record<string, unknown> {
    const s = this.#lanes[lane]!;
    const hw = this.#hw[lane]!;
    const cfg = this.#laneTable[lane]!;
    const mode = this.#effectiveMode(lane);
    const nowUs = this.#sysUsNow();
    const raw = this.#sample(lane, nowUs);
    const baseline = this.#baseline(lane, nowUs);
    const thrLow = Math.round(baseline * (1000 - cfg.thrDropPermille) / 1000);
    const thrHigh = Math.min(0xffff, thrLow + Math.round(baseline * cfg.hystPermille / 1000));
    let rawMin = raw;
    let rawMax = raw;
    const times = this.#sampleTimes(lane, s.winFromUs, nowUs, WINDOW_MAX_EVAL);
    for (const p of s.passes) {
      if (p.endUs >= s.winFromUs && p.startUs <= nowUs) times.push(Math.max(s.winFromUs, Math.min(nowUs, (p.startUs + p.endUs) / 2)));
    }
    for (const t of times) {
      const v = this.#sample(lane, t);
      if (v < rawMin) rawMin = v;
      if (v > rawMax) rawMax = v;
    }
    s.winFromUs = nowUs + 1;
    let flags = 0;
    if (s.configured && s.syncOk) flags |= LaneStatusFlag.SYNC_LOCKED | LaneStatusFlag.SYNC_ALIGNED;
    if (!s.syncOk) flags |= LaneStatusFlag.SYNC_LOST;
    if (!hw.sensorBroken) flags |= LaneStatusFlag.SENSOR_OK;
    if (s.configured) flags |= LaneStatusFlag.CONFIG_VALID;
    if (s.base && !s.learning) flags |= LaneStatusFlag.CALIBRATED;
    if (s.base && raw < thrLow) flags |= LaneStatusFlag.BLOCKED;
    if (s.droppedEvents > 0) flags |= LaneStatusFlag.FIFO_OVERFLOW;
    return {
      lane, state: !s.configured ? LaneBusState.ENUMERATED : mode === SamplingMode.OFF ? LaneBusState.IDLE : LaneBusState.SAMPLING,
      samplingMode: mode, fifoCount: 0, flags, droppedEvents: s.droppedEvents & 0xffff, syncShifts: s.syncShifts & 0xffff,
      badWrites: s.badWrites, lateActions: s.lateActions, lastError: s.fault, resetCause: s.resetCause, faultId: 0,
      uptimeMs: Math.floor(this.scheduler.now() - s.bootMs) >>> 0, raw, baseline, thrLow, thrHigh,
      noise: this.#laneNoise(lane), reference: s.reference, rawMin, rawMax,
    };
  }

  #startRaw(lane: number, decimation: number): void {
    const prevMode = this.#sampling[lane] ?? SamplingMode.ON;
    const timer = this.scheduler.setInterval(() => this.#rawChunk(), RAW_CHUNK_MS);
    this.#raw = { lane, decimation, prevMode, chunkSeq: 0, fromUs: this.#sysUsNow(), timer };
  }

  #stopRaw(): void {
    if (!this.#raw) return;
    this.scheduler.clearInterval(this.#raw.timer);
    this.#sampling[this.#raw.lane] = this.#raw.prevMode;
    this.#raw = null;
  }

  #rawChunk(): void {
    const r = this.#raw;
    if (!r) return;
    const periodUs = this.#laneTable[r.lane]!.samplePeriodUs * r.decimation;
    const nowUs = this.#sysUsNow();
    while (r.fromUs + periodUs <= nowUs) {
      const samples: number[] = [];
      const firstUs = r.fromUs;
      while (samples.length < RAW_MAX_SAMPLES && r.fromUs + periodUs <= nowUs) {
        samples.push(this.#sample(r.lane, r.fromUs));
        r.fromUs += periodUs;
      }
      this.#stream(STR_RAW_SAMPLES, MsgId.STR_RAW_SAMPLES, {
        lane: r.lane, reserved: 0, chunkSeq: r.chunkSeq, periodUs, firstUs: BigInt(firstUs), samples,
      });
      r.chunkSeq = (r.chunkSeq + 1) & 0xffff;
    }
  }

  // ---- start sequence ----------------------------------------------------------------------------

  #startSequence(req: Record<string, unknown>, reply: (st: number, d?: StructDesc, b?: object) => void): void {
    const lights = req['lights'] as number;
    const stepMs = req['stepMs'] as number;
    const holdMin = req['holdMinMs'] as number;
    const holdMax = req['holdMaxMs'] as number;
    const leadMs = req['leadMs'] as number;
    const riseMs = (req['pulseRiseMs'] as number) || SEQ_DEFAULT_PULSE_MS;
    const pulses = req['attentionPulses'] as number;
    const arm = req['armSampling'] as number;
    if (this.#seq) { reply(Status.ERR_SEQ_RUNNING); return; }
    if (lights === 0 || lights > MAX_LANES || stepMs === 0 || holdMin > holdMax || arm > 1) { reply(Status.ERR_INVALID_ARG); return; }
    if (leadMs !== 0 && leadMs < SEQ_MIN_LEAD_MS) { reply(Status.ERR_TOO_LATE); return; }
    const present: number[] = [];
    for (let l = 1; l <= MAX_LANES; l++) if (this.#lanes[l]!.present && this.#lanes[l]!.configured) present.push(l);
    if (present.length === 0) { reply(Status.ERR_LANE_ABSENT); return; }
    const n = Math.min(lights, present.length);
    const used = present.slice(0, n);
    if (used.some((l) => this.#failSchedule.has(l))) {
      this.#failSchedule.clear();
      reply(Status.ERR_LANE_NACK);
      return;
    }

    const holdUs = this.#rng.int(holdMin * 1000, holdMax * 1000);
    const t0Us = this.#sysUsNow() + (leadMs || SEQ_MIN_LEAD_MS) * 1000;
    const riseUs = riseMs * 1000;
    const stepUs = stepMs * 1000;
    const troughUs = t0Us + 2 * pulses * riseUs;
    const tLitUs = troughUs + (n - 1) * stepUs;
    const toutUs = tLitUs + holdUs;
    const seq: Sequence = { t0Us, toutUs, lanes: used, timers: [] };
    const at = (us: number, fn: () => void): void => {
      seq.timers.push(this.scheduler.setTimeout(fn, Math.max(0, this.#msAtUs(us) - this.scheduler.now())));
    };
    const on = (lane: number): void => {
      this.#lanes[lane]!.led = { mode: LedMode.ON, level: 1000, blinkPeriodMs: 0, blinkOnPermille: 0, blinkCount: 0 };
    };
    const off = (lane: number): void => { this.#lanes[lane]!.led = { ...LED_OFF }; };
    // Same timeline as seq_planner.c: all lit lanes breathe from T0; the far-end lane (last of `used`) stays on at the
    // last peak, the others go off at the next trough; then one lane per step toward lane 1; all off at Tout.
    used.forEach((lane, idx) => {
      const j = n - 1 - idx;   // 0 = far end
      if (pulses > 0) {
        at(t0Us, () => {
          this.#lanes[lane]!.led = { ...LED_OFF, mode: LedMode.ON, pulse: { startUs: t0Us, riseMs } };
        });
        if (j === 0) at(troughUs - riseUs, () => on(lane));
        else { at(troughUs, () => off(lane)); at(troughUs + j * stepUs, () => on(lane)); }
      } else {
        at(t0Us + j * stepUs, () => on(lane));
      }
    });
    if (arm) at(t0Us, () => { for (const l of present) if (this.#sampling[l] === SamplingMode.OFF) this.#sampling[l] = SamplingMode.ON; });
    at(toutUs, () => {
      for (const l of used) off(l);
      this.#seq = null;
      const jitterMs = this.#rng.range(0, 0.8);
      this.#later(jitterMs, () => {
        this.#publish(EVT_LIGHTS_OUT, { lights: n, reserved: new Uint8Array(3), toutUs: BigInt(toutUs) });
      });
      for (const c of this.#cars.slice(1)) c.lightsOut(this.#msAtUs(toutUs));
    });
    this.#seq = seq;
    this.#publish(EVT_SEQ_STARTED, { lights: n, armSampling: arm, stepMs, t0Us: BigInt(t0Us) });
    reply(Status.OK, START_SEQUENCE_RSP, { reserved: 0, t0Us: BigInt(t0Us) });
    for (const c of this.#cars.slice(1)) c.sequenceStarted(this.#msAtUs(t0Us), this.#msAtUs(toutUs));
  }

  #abortSequence(reason: number): void {
    const seq = this.#seq;
    if (!seq) return;
    for (const t of seq.timers) this.scheduler.clearTimeout(t);
    this.#seq = null;
    for (const l of seq.lanes) {
      this.#lanes[l]!.led = { mode: LedMode.BLINK, level: 1000, blinkPeriodMs: 300, blinkOnPermille: 500, blinkCount: 3 };
    }
    this.#publish(EVT_SEQ_ABORTED, { reason, reserved: new Uint8Array(3), atUs: this.#sysUsBig() });
    for (const c of this.#cars.slice(1)) c.sequenceAborted();
  }

  // ---- nodes -------------------------------------------------------------------------------------

  #nodes(): NodeInfo[] {
    const fw = SIM_APP_FW;
    const nodes: NodeInfo[] = [{
      nodeClass: NodeClass.CTRL, index: 0, boardType: BoardType.CONTROL,
      state: NodeState.OK, fwMajor: fw.major, fwMinor: fw.minor, fwPatch: fw.patch, protoMinor: PROTO.minor,
      fwGitRev: fw.gitRev, imageCrc: 0,   // no image header since 0.4: unknown
    }];
    // Like host_manager: the BLE node is always listed; its state follows the
    // bridge liveness (ctrlPolicyBleNodeState: ABSENT / OK / mismatch = FAULT).
    const bleState = this.bleBridge === BleBridgeState.ABSENT ? NodeState.ABSENT
      : this.bleBridge === BleBridgeState.VERSION_MISMATCH ? NodeState.FAULT : NodeState.OK;
    nodes.push({
      nodeClass: NodeClass.BLE, index: 0, boardType: BoardType.BLE_BRIDGE, state: bleState,
      fwMajor: SIM_BLE_FW.major, fwMinor: SIM_BLE_FW.minor, fwPatch: SIM_BLE_FW.patch, protoMinor: SIM_BLE_LINK_MINOR,
      fwGitRev: SIM_BLE_FW.gitRev, imageCrc: 0,
    });
    for (let lane = 1; lane <= MAX_LANES; lane++) {
      const s = this.#lanes[lane]!;
      if (!s.present) continue;
      nodes.push({
        // Like lane_manager: UPDATING while the config push runs, OK once configured.
        nodeClass: NodeClass.LANE, index: lane, boardType: BoardType.LANE,
        state: s.fault ? NodeState.FAULT : s.configured ? NodeState.OK : NodeState.UPDATING,
        fwMajor: SIM_LANE_FW.major, fwMinor: SIM_LANE_FW.minor, fwPatch: SIM_LANE_FW.patch, protoMinor: SIM_LANE_PROTO_MINOR,
        fwGitRev: SIM_LANE_FW.gitRev, imageCrc: 0,
      });
    }
    return nodes;
  }
}
