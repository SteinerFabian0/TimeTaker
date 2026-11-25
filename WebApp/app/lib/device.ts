// The app's one connection to a time taker (GUI_SPEC §2.1, §10): USB, Bluetooth
// or the simulator → DeviceManager (src/device) → HealthMonitor (src/health).
// It fills the shell's signals every page reads:
//   connection  link + overall health (the Status icon in the rail)
//   trackLink   the device for pages that act on the track (lights, races)
// and the Status page's own: `device` (state, nodes, STATUS, health report),
// `samplesTick` (lane sensor history changed) and `trace` (decoded events).
//
// The chosen mode is remembered per browser: after a reload the app reconnects
// to the simulator, or to a USB / Bluetooth time taker this browser was
// granted before, without asking again.
import { signal } from '@preact/signals';
import { DeviceManager } from '../../src/device/deviceManager.ts';
import type { ManagerState, TransportSource } from '../../src/device/deviceManager.ts';
import type { DeviceEvent } from '../../src/device/deviceSession.ts';
import { emptyNodeModel, presentLanes } from '../../src/device/nodeModel.ts';
import type { NodeModel } from '../../src/device/nodeModel.ts';
import { browserStorage, loadClientId, loadClientName } from '../../src/device/clientId.ts';
import { SimDevice } from '../../src/device/simDevice.ts';
import { HealthMonitor, MonitorBusyError } from '../../src/health/monitor.ts';
import type { CalibrationOutcome } from '../../src/health/monitor.ts';
import type { HealthReport } from '../../src/health/health.ts';
import { SerialTransport, isWebSerialAvailable } from '../../src/transport/serialTransport.ts';
import { BleTransport, isWebBluetoothAvailable } from '../../src/transport/bleTransport.ts';
import type { Transport } from '../../src/transport/transport.ts';
import {
  BleLinkState, ButtonId, CalibrateResult, LaneEventType, LaneState, MsgId, PressKind, ResetCause,
} from '../../src/protocol/generated.ts';
import type { HelloRsp, StatusRsp, NodeInfo } from '../../src/protocol/generated.ts';
import { connection, trackLink, raceSessionActive, type Link } from '../shell/connection.ts';

export type Mode = 'off' | 'device' | 'sim';

export interface DeviceView {
  readonly mode: Mode;
  readonly state: ManagerState;
  /** usb / ble / sim while connected, else offline. */
  readonly link: Link;
  /** The transport's own name ("TimeTaker (USB)"). */
  readonly label: string | null;
  /** Why the last connection ended or failed. */
  readonly reason: string | null;
  readonly hello: HelloRsp | null;
  readonly nodes: NodeModel;
  readonly status: StatusRsp | null;
  readonly report: HealthReport | null;
}

const MODE_KEY = 'tt.deviceMode';
function readMode(): Mode {
  try { const v = localStorage.getItem(MODE_KEY); return v === 'device' || v === 'sim' ? v : 'off'; } catch { return 'off'; }
}
function writeMode(m: Mode): void {
  try { localStorage.setItem(MODE_KEY, m); } catch { /* not remembered */ }
}

/** A node's firmware version: 1.2.3, with the git revision when it has one (1.2.3+abc1234); '–' without a node. */
export const fwVersion = (n: NodeInfo | null | undefined): string =>
  n ? `${n.fwMajor}.${n.fwMinor}.${n.fwPatch}${n.fwGitRev ? '+' + n.fwGitRev.toString(16).padStart(7, '0') : ''}` : '–';

export const device = signal<DeviceView>({
  mode: 'off', state: 'disconnected', link: 'offline', label: null, reason: null,
  hello: null, nodes: emptyNodeModel(), status: null, report: null,
});
/** Bumped (at most every REFRESH_MS) when lane samples or markers arrive: graphs read monitor() then. */
export const samplesTick = signal(0);

export interface TraceEntry {
  readonly key: number;
  /** Host time (Date.now()). */
  readonly at: number;
  readonly kind: 'event' | 'note';
  readonly evSeq: number | null;
  readonly name: string;
  readonly text: string;
}
/** Newest first, at most TRACE_MAX. */
export const trace = signal<readonly TraceEntry[]>([]);
const TRACE_MAX = 300;

export const canUsb = isWebSerialAvailable();
export const canBle = isWebBluetoothAvailable();

let manager: DeviceManager | null = null;
let mon: HealthMonitor | null = null;
let simDevice: SimDevice | null = null;
let mode: Mode = 'off';
let unsubs: (() => void)[] = [];
const raceListeners = new Set<(e: DeviceEvent) => void>();

export const monitor = (): HealthMonitor | null => mon;
export const sim = (): SimDevice | null => (mode === 'sim' ? simDevice : null);

// ---- view updates ---------------------------------------------------------------------

function patch(p: Partial<DeviceView>): void {
  device.value = { ...device.value, ...p };
  syncShell();
}

const REFRESH_MS = 250;
let refreshTimer = 0;
/** Coalesces the monitor's many updates (24 samples/s with six lanes) into one re-render. */
function scheduleRefresh(): void {
  if (refreshTimer) return;
  refreshTimer = window.setTimeout(() => {
    refreshTimer = 0;
    if (!mon || !manager) return;
    samplesTick.value++;
    patch({ report: mon.report(), status: mon.status, nodes: mon.nodes });
  }, REFRESH_MS);
}

/** The lane count reported to the app only after it held this long (lanes join one by one). */
const LANES_STABLE_MS = 3000;
let lanesSeen = { n: 0, since: 0 };
let reportedLanes = 0;
let lanesTimer = 0;

function stableLanes(nodes: NodeModel): number {
  const present = presentLanes(nodes);
  const settled = present.length > 0 && nodes.lanes.every(l => l.phase !== 'joined');
  const n = settled ? present.length : present.length === 0 ? 0 : -1;   // -1: still joining
  const now = Date.now();
  if (n !== lanesSeen.n) lanesSeen = { n, since: now };
  if (n >= 0 && n !== reportedLanes) {
    if (now - lanesSeen.since >= LANES_STABLE_MS) reportedLanes = n;
    else if (!lanesTimer) lanesTimer = window.setTimeout(() => { lanesTimer = 0; syncShell(); }, LANES_STABLE_MS);
  }
  return reportedLanes;
}

function syncShell(): void {
  const d = device.value;
  const connected = d.state === 'connected';
  const overall = d.report?.overall ?? 'fail';
  const c = connection.value;
  if (c.link !== d.link || c.overall !== overall) connection.value = { link: d.link, overall, powerPack: false };

  const m = manager;
  if (!connected || !m) {
    if (trackLink.value) trackLink.value = null;
    return;
  }
  const lanes = stableLanes(d.nodes);
  const seqRunning = d.nodes.seqRunning;
  const cur = trackLink.value;
  if (cur && cur.api === m.api && cur.lanes === lanes && cur.seqRunning === seqRunning) return;
  trackLink.value = {
    api: m.api, lanes, seqRunning,
    subscribe: (fn) => { raceListeners.add(fn); return () => { raceListeners.delete(fn); }; },
  };
}

// ---- trace ----------------------------------------------------------------------------

let traceKey = 0;
function addTrace(e: Omit<TraceEntry, 'key' | 'at'>): void {
  const next = [{ ...e, key: ++traceKey, at: Date.now() }, ...trace.value];
  if (next.length > TRACE_MAX) next.length = TRACE_MAX;
  trace.value = next;
}

function nameOf(table: Readonly<Record<string, number>>, v: unknown): string {
  for (const [k, x] of Object.entries(table)) if (x === v) return k;
  return String(v);
}
const hex32 = (n: unknown): string => `0x${((n as number) >>> 0).toString(16).padStart(8, '0')}`;

function plain(v: unknown): string {
  if (typeof v === 'bigint') return v.toString();
  if (v instanceof Uint8Array) return Array.from(v, b => b.toString(16).padStart(2, '0')).join('');
  if (typeof v === 'object' && v !== null) return JSON.stringify(v, (_k, x: unknown) => (typeof x === 'bigint' ? x.toString() : x));
  return String(v);
}

/** One line per event: the fields that matter, enums by name. Technical on purpose (GUI_SPEC §10). */
export function eventText(e: DeviceEvent): string {
  const v = e.value;
  if (!v) return 'unknown to this app';
  switch (e.msgId) {
    case MsgId.EVT_BOOT: return `${nameOf(ResetCause, v['resetCause'])} reset · boot ${hex32(v['bootId'])}`;
    case MsgId.EVT_BUTTON: return `${nameOf(ButtonId, v['button'])} ${nameOf(PressKind, v['press'])}`;
    case MsgId.EVT_LANE_STATE:
      return `lane ${String(v['lane'])} ${nameOf(LaneState, v['state'])}${v['status'] ? ` (status ${String(v['status'])})` : ''}`;
    case MsgId.EVT_CROSSING:
      return `lane ${String(v['lane'])} ${nameOf(LaneEventType, v['type'])} · dwell ${((v['dwell10us'] as number) / 100).toFixed(1)} ms`
        + ` · depth ${String(v['depth'])} · #${String(v['laneSeq'])}`;
    case MsgId.EVT_BLE_LINK: return `${nameOf(BleLinkState, v['state'])} · MTU ${String(v['attMtu'])}`;
    case MsgId.EVT_LANE_CALIBRATED:
      return `lane ${String(v['lane'])} ${nameOf(CalibrateResult, v['result'])} · reference ${String(v['reference'])} · noise ${String(v['noise'])}`;
    default:
      return Object.entries(v).filter(([k]) => k !== 'evSeq').map(([k, x]) => `${k} ${plain(x)}`).join(' · ');
  }
}

// ---- connect / disconnect -------------------------------------------------------------

function sourcesFor(m: Mode): TransportSource[] {
  if (m === 'sim') {
    simDevice ??= new SimDevice({ latencyMs: 3 });
    const s = simDevice;
    return [{ kind: 'usb', find: async () => s.transport('usb') }];
  }
  return [
    { kind: 'usb', find: () => SerialTransport.remembered() },
    { kind: 'ble', find: () => BleTransport.remembered() },
  ];
}

async function teardown(): Promise<void> {
  for (const u of unsubs) u();
  unsubs = [];
  mon?.stop();
  mon = null;
  const m = manager;
  manager = null;
  await m?.stop().catch(() => undefined);
}

/** A fresh manager + monitor for this mode (not started). */
async function build(m: Mode): Promise<DeviceManager> {
  await teardown();
  mode = m;
  writeMode(m);
  const storage = browserStorage();
  const mgr = new DeviceManager({
    sources: sourcesFor(m), clientId: loadClientId(storage), clientName: loadClientName(storage),
  }, {
    onState: (state, kind, reason) => {
      if (manager !== mgr) return;
      const link: Link = state !== 'connected' ? 'offline' : mode === 'sim' ? 'sim' : kind ?? 'usb';
      patch({ state, link, label: mgr.session?.transport.label ?? null, reason: state === 'disconnected' ? reason ?? null : null });
      if (state === 'disconnected' && reason && reason !== 'stopped') addTrace({ kind: 'note', evSeq: null, name: 'link', text: reason });
    },
    onHello: (hello) => { if (manager === mgr) patch({ hello }); },
    onNodes: () => { if (manager === mgr) scheduleRefresh(); },
    onEvent: (e) => {
      if (manager !== mgr) return;
      addTrace({ kind: 'event', evSeq: e.evSeq, name: e.name, text: eventText(e) });
      for (const fn of raceListeners) { try { fn(e); } catch { /* a page's listener must not stop the stream */ } }
    },
    onBoot: (b, prev) => {
      if (prev !== null) addTrace({ kind: 'note', evSeq: null, name: 'boot', text: `device rebooted: ${hex32(prev)} → ${hex32(b)}` });
    },
    onGap: (_b, from, to) => addTrace({ kind: 'note', evSeq: null, name: 'gap', text: `events ${from}…${to - 1} lost (overwritten on the device)` }),
  });
  manager = mgr;
  // busy: the race page owns the track; the monitor then polls slowly and refuses calibration.
  mon = HealthMonitor.attach(mgr, { busy: () => raceSessionActive.peek() });
  unsubs.push(mon.subscribe((u) => { scheduleRefresh(); if (u.type === 'report') autoLanePower(); }));
  patch({ mode: m, state: mgr.state, link: 'offline', label: null, reason: null, hello: null, nodes: mgr.nodes, status: null, report: mon.report() });
  return mgr;
}

/**
 * The lane rail is off after every boot of the control module: the app switches
 * it on once per boot, as soon as STATUS shows it off. Switched off by hand
 * afterwards (Status page), it stays off until the next boot.
 */
let autoPoweredBoot: number | null = null;
function autoLanePower(): void {
  const st = mon?.status;
  const bootId = manager?.resumePoint?.bootId ?? device.value.hello?.bootId ?? null;
  if (!manager || manager.state !== 'connected' || !st || bootId === null) return;
  if (st.lanesPowered === 1) { autoPoweredBoot = bootId; return; }
  if (autoPoweredBoot === bootId) return;
  autoPoweredBoot = bootId;
  void manager.api.lanePower(true).then(() => scheduleRefresh()).catch(() => { autoPoweredBoot = null; });
}

async function connectWith(m: Mode, pick: () => Promise<Transport>): Promise<void> {
  const transport = await pick();             // the browser's picker first, inside the click
  const mgr = await build(m);
  const opened = mgr.connectWith(transport);
  mgr.start();                                // keeps reconnecting from now on
  try {
    await opened;
  } catch (err) {
    patch({ reason: err instanceof Error ? err.message : String(err) });
  }
}

/** Shows the browser's USB port picker (call from a click). Throws when the user cancels. */
export function connectUsb(): Promise<void> {
  return connectWith('device', () => SerialTransport.request());
}

/** Shows the browser's Bluetooth chooser (call from a click). Throws when the user cancels. */
export function connectBle(): Promise<void> {
  return connectWith('device', () => BleTransport.request());
}

export async function connectSim(): Promise<void> {
  const mgr = await build('sim');
  mgr.start();
}

export async function disconnect(): Promise<void> {
  await teardown();
  mode = 'off';
  writeMode('off');
  patch({ mode: 'off', state: 'disconnected', link: 'offline', label: null, reason: null, hello: null, status: null, report: null });
  addTrace({ kind: 'note', evSeq: null, name: 'link', text: 'disconnected' });
}

// ---- actions ----------------------------------------------------------------------------

export async function lanePower(on: boolean): Promise<void> {
  if (!manager) throw new Error('not connected');
  await manager.api.lanePower(on);
  scheduleRefresh();
}

export async function selfCheck(): Promise<HealthReport | null> {
  if (!mon) return null;
  const r = await mon.runSelfCheck();
  scheduleRefresh();
  return r;
}

export type CalibrateError = 'busy' | 'failed';

/** LEARN (lanes free!) or FORGET; resolves with each lane's outcome. */
export async function calibrate(lanes: readonly number[] | 'all', forget = false): Promise<CalibrationOutcome[] | CalibrateError> {
  if (!mon) return 'failed';
  try {
    const out = forget ? await mon.forget(lanes) : await mon.calibrate(lanes);
    scheduleRefresh();
    return out;
  } catch (err) {
    return err instanceof MonitorBusyError ? 'busy' : 'failed';
  }
}

/** Round trip of the current session (ms), null if unknown. */
export function rttMs(): number | null {
  return manager?.session?.rttMs() ?? null;
}

// ---- start --------------------------------------------------------------------------------

const saved = readMode();
if (saved === 'sim') void connectSim();
else if (saved === 'device') void build('device').then(m => m.start());
