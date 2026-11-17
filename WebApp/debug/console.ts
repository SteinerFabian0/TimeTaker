/*
 * TimeTaker debug console: the bring-up window onto the control module.
 *
 * Connects over USB (Web Serial) or to the simulator, then shows:
 *   - the session: bootId, protocol, the live device clock, RTT, event position;
 *   - the node list;
 *   - every event (evSeq, device time, decoded);
 *   - a trace of the raw frames.
 * Controls send the step-2 requests: LANE_POWER, NODE_LIST, REBOOT.
 *
 * Plain DOM, no framework: this page is a tool, not the app (§11).
 */

import { DeviceError, DeviceSession } from '../src/device/deviceSession.ts';
import type { DeviceEvent, ResumePoint, SessionState } from '../src/device/deviceSession.ts';
import { SimDevice } from '../src/device/simDevice.ts';
import { isWebSerialAvailable, SerialTransport } from '../src/transport/serialTransport.ts';
import type { Transport } from '../src/transport/transport.ts';
import {
  BoardType, ButtonId, LANE_POWER_REQ, LANE_POWER_RSP, LaneEventFlag, LaneEventType, LaneState, MsgId,
  NODE_LIST_REQ, NODE_LIST_RSP, NodeClass, NodeState, PressKind, REBOOT_REQ, REBOOT_RSP, RebootTarget, ResetCause,
  Transport as HpTransport,
} from '../src/protocol/generated.ts';
import type { NodeInfo, NodeListRsp } from '../src/protocol/generated.ts';

// ---- tiny DOM helpers ---------------------------------------------------------

const $ = <T extends HTMLElement>(id: string): T => {
  const el = document.getElementById(id);
  if (!el) throw new Error(`#${id} missing`);
  return el as T;
};

function setText(id: string, text: string): void {
  $(id).textContent = text;
}

function nameOf(table: Readonly<Record<string, number>>, value: unknown): string {
  for (const [k, v] of Object.entries(table)) if (v === value) return k;
  return String(value);
}

function flagNames(table: Readonly<Record<string, number>>, bits: number): string {
  const set = Object.entries(table).filter(([, v]) => (bits & v) !== 0).map(([k]) => k);
  return set.length ? set.join('|') : '–';
}

const hex = (b: Uint8Array): string => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join(' ');
const hex32 = (n: number): string => `0x${(n >>> 0).toString(16).padStart(8, '0')}`;

function fmtUs(us: unknown): string {
  if (typeof us !== 'bigint') return '–';
  const ms = us / 1000n;
  const s = ms / 1000n;
  return `${s}.${(ms % 1000n).toString().padStart(3, '0')} s`;
}

// ---- state --------------------------------------------------------------------

let session: DeviceSession | null = null;
let sim: SimDevice | null = null;
let resume: ResumePoint | null = null;
let framesIn = 0;
let framesOut = 0;
let eventCount = 0;

function clientId(): Uint8Array {
  try {
    const stored = localStorage.getItem('timetaker.clientId');
    if (stored && /^[0-9a-f]{32}$/.test(stored)) {
      return new Uint8Array(stored.match(/../g)!.map((h) => parseInt(h, 16)));
    }
    const id = crypto.getRandomValues(new Uint8Array(16));
    localStorage.setItem('timetaker.clientId', Array.from(id, (b) => b.toString(16).padStart(2, '0')).join(''));
    return id;
  } catch {
    return crypto.getRandomValues(new Uint8Array(16));
  }
}

// ---- rendering ------------------------------------------------------------------

function showState(state: SessionState, reason?: string): void {
  $('stateDot').className = `dot ${state}`;
  const label = session?.transport.label ?? '';
  setText('stateText', state === 'open' ? `connected · ${label}` : state === 'connecting' ? 'connecting…'
    : state === 'closed' ? `disconnected${reason ? ` · ${reason}` : ''}` : 'not connected');
  const open = state === 'open';
  for (const id of ['btnLaneOn', 'btnLaneOff', 'btnNodes', 'btnReboot']) $<HTMLButtonElement>(id).disabled = !open;
  $<HTMLButtonElement>('btnDisconnect').disabled = !(open || state === 'connecting');
  $<HTMLButtonElement>('btnUsb').disabled = open || state === 'connecting';
  $<HTMLButtonElement>('btnSim').disabled = open || state === 'connecting';
}

function showError(err: unknown): void {
  const el = $('dError');
  el.hidden = false;
  el.textContent = err instanceof DeviceError ? `${err.msgName} → ${err.message.split(': ').pop()}`
    : err instanceof Error ? err.message : String(err);
}

function clearError(): void {
  $('dError').hidden = true;
}

function eventDetails(e: DeviceEvent): string {
  const v = e.value;
  if (!v) return `${e.name} (unknown to this app)`;
  switch (e.msgId) {
    case MsgId.EVT_BOOT:
      return `${nameOf(ResetCause, v['resetCause'])} reset · boot ${hex32(v['bootId'] as number)}`;
    case MsgId.EVT_BUTTON:
      return `${nameOf(ButtonId, v['button'])} ${nameOf(PressKind, v['press'])}`;
    case MsgId.EVT_LANE_STATE:
      return `lane ${String(v['lane'])} ${nameOf(LaneState, v['state'])}${v['status'] ? ` (status ${String(v['status'])})` : ''}`;
    case MsgId.EVT_CROSSING:
      return `lane ${String(v['lane'])} ${nameOf(LaneEventType, v['type'])} · dwell ${((v['dwell10us'] as number) / 100).toFixed(1)} ms`
        + ` · depth ${String(v['depth'])} · #${String(v['laneSeq'])} · flags ${flagNames(LaneEventFlag, v['flags'] as number)}`;
    default:
      return JSON.stringify(v, (_k, x: unknown) => (typeof x === 'bigint' ? x.toString() : x));
  }
}

function eventTime(e: DeviceEvent): unknown {
  return e.value?.['tEntryUs'] ?? e.value?.['sysUs'];
}

function addEvent(e: DeviceEvent): void {
  eventCount++;
  const tr = document.createElement('tr');
  const cells = [String(e.evSeq), fmtUs(eventTime(e)), e.name, eventDetails(e)];
  cells.forEach((text, i) => {
    const td = document.createElement('td');
    td.textContent = text;
    if (i < 2) td.className = 'num';
    tr.appendChild(td);
  });
  const body = $('events');
  body.prepend(tr);
  while (body.children.length > 500) body.lastElementChild?.remove();
  setText('evCount', `${eventCount} received`);
}

function addNote(text: string): void {
  const tr = document.createElement('tr');
  const td = document.createElement('td');
  td.colSpan = 4;
  td.className = 'note';
  td.textContent = text;
  tr.appendChild(td);
  $('events').prepend(tr);
}

function addFrame(dir: 'in' | 'out', raw: Uint8Array): void {
  if (dir === 'in') framesIn++; else framesOut++;
  const msgId = raw[0] ?? 0;
  if ((msgId === MsgId.PING) && !$<HTMLInputElement>('chkFrames').checked) return;
  const div = document.createElement('div');
  div.className = dir;
  div.textContent = `${dir === 'in' ? '←' : '→'} ${hex(raw)}`;
  const box = $('frames');
  box.prepend(div);
  while (box.children.length > 200) box.lastElementChild?.remove();
}

function renderNodes(nodes: readonly NodeInfo[]): void {
  const body = $('nodes');
  body.replaceChildren();
  for (const n of nodes) {
    const tr = document.createElement('tr');
    const node = n.nodeClass === NodeClass.LANE ? `Lane ${n.index}` : nameOf(NodeClass, n.nodeClass);
    const fw = `${n.fwMajor}.${n.fwMinor}.${n.fwPatch}+${n.fwGitRev ? n.fwGitRev.toString(16).padStart(7, '0') : 'nogit'}`;
    for (const text of [node, nameOf(BoardType, n.boardType), nameOf(NodeState, n.state), fw]) {
      const td = document.createElement('td');
      td.textContent = text;
      tr.appendChild(td);
    }
    body.appendChild(tr);
  }
}

function tick(): void {
  setText('dFrames', `${framesIn} in · ${framesOut} out`);
  if (!session || session.state !== 'open') return;
  setText('dClock', fmtUs(session.sysUsNow()));
  const rtt = session.rttMs();
  setText('dRtt', rtt === null ? '–' : `${rtt.toFixed(1)} ms`);
  const rp = session.resumePoint;
  setText('dSeq', rp ? String(rp.nextEvSeq) : '–');
}

// ---- actions --------------------------------------------------------------------

async function connect(transport: Transport): Promise<void> {
  clearError();
  const s = new DeviceSession(transport, { clientId: clientId(), clientName: 'Debug console' }, {
    onState: (state, reason) => {
      showState(state, reason);
      if (state === 'closed' && session === s) resume = s.resumePoint;
    },
    onEvent: addEvent,
    onFrame: (f) => addFrame(f.dir, f.raw),
    onBoot: (bootId, previous) => {
      if (previous !== null) addNote(`device rebooted: boot ${hex32(previous)} → ${hex32(bootId)}`);
    },
    onGap: (_b, from, to) => addNote(`events ${from}…${to - 1} were lost (overwritten on the device)`),
  }, resume);
  session = s;
  try {
    const hello = await s.connect();
    setText('dTransport', `${s.transport.label} · ${nameOf(HpTransport, hello.transport)}`);
    setText('dBoot', hex32(hello.bootId));
    setText('dProto', `${hello.protoMajor}.${hello.protoMinor}`);
    await refreshNodes();
  } catch (err) {
    showError(err);
  }
}

async function refreshNodes(): Promise<void> {
  if (!session) return;
  try {
    const rsp = await session.request<NodeListRsp>(NODE_LIST_REQ, {}, MsgId.NODE_LIST, NODE_LIST_RSP);
    renderNodes(rsp.nodes);
  } catch (err) {
    showError(err);
  }
}

async function lanePower(on: boolean): Promise<void> {
  if (!session) return;
  clearError();
  try {
    await session.request(LANE_POWER_REQ, { on: on ? 1 : 0 }, MsgId.LANE_POWER, LANE_POWER_RSP);
    setTimeout(() => { void refreshNodes(); }, 80);
  } catch (err) {
    showError(err);
  }
}

$('btnUsb').addEventListener('click', () => {
  void (async () => {
    try {
      await connect(await SerialTransport.request());
    } catch (err) {
      showError(err);
    }
  })();
});

$('btnSim').addEventListener('click', () => {
  if (!sim) sim = new SimDevice({ latencyMs: 3 });
  $('simPanel').hidden = false;
  void connect(sim.transport());
});

$('btnDisconnect').addEventListener('click', () => { void session?.close('closed by you'); });
$('btnLaneOn').addEventListener('click', () => { void lanePower(true); });
$('btnLaneOff').addEventListener('click', () => { void lanePower(false); });
$('btnNodes').addEventListener('click', () => { void refreshNodes(); });
$('btnReboot').addEventListener('click', () => {
  void session?.request(REBOOT_REQ, { target: RebootTarget.APP }, MsgId.REBOOT, REBOOT_RSP).catch(showError);
});

document.querySelectorAll<HTMLButtonElement>('[data-btn]').forEach((b) => {
  b.addEventListener('click', () => sim?.pressButton(Number(b.dataset['btn']), Number(b.dataset['press'])));
});
$('btnCars').addEventListener('click', () => {
  if (!sim) return;
  sim.setCars(!sim.cars);
  setText('btnCars', `Cars: ${sim.cars ? 'on' : 'off'}`);
});
$('btnDrop').addEventListener('click', () => sim?.dropNextEvents(3));
$('btnPowerCycle').addEventListener('click', () => sim?.powerCycle());

if (!isWebSerialAvailable()) {
  $<HTMLButtonElement>('btnUsb').disabled = true;
  $<HTMLButtonElement>('btnUsb').title = 'Web Serial needs Chrome or Edge on a desktop OS';
}
showState('idle');
setInterval(tick, 100);
