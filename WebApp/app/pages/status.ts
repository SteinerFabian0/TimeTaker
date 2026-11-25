// Status (GUI_SPEC §10): the connection, the HealthReport made visual (one
// card per part, a fixed row per check in catalogue order), the lane sensor
// readout with calibration, and the raw event trace. Technical tone is fine.
import { useEffect, useState } from 'preact/hooks';
import { signal, effect } from '@preact/signals';
import { html } from '../lib/html.ts';
import { t, type Key } from '../i18n/i18n.ts';
import { lang, devTools } from '../lib/prefs.ts';
import { trackLanes } from '../lib/trackLanes.ts';
import {
  device, fwVersion, samplesTick, trace, monitor, sim, canUsb, canBle,
  connectUsb, connectBle, connectSim, disconnect, lanePower, selfCheck, calibrate, rttMs,
} from '../lib/device.ts';
import { CHECKS, worst, type Check, type CheckLevel } from '../../src/health/health.ts';
import { presentLanes } from '../../src/device/nodeModel.ts';
import { BleBridgeState, type NodeInfo } from '../../src/protocol/generated.ts';
import { realScheduler } from '../../src/util/clock.ts';
import { MONITOR_DEFAULTS, type LaneSample, type LaneMarker, type CalibrationOutcome } from '../../src/health/monitor.ts';
import { AREAS } from '../shell/areas.ts';
import { PageHeader, Panel, Button, ConfirmDialog } from '../ui/ui.ts';

const DESCRIPTION = new Map(CHECKS.map(c => [c.id, c.description]));

/** Whether the lanes show their check lists (per browser). Collapsed, every lane graph fits on screen. */
const OPEN_KEY = 'tt.statusChecksOpen';
const checksOpen = signal<boolean>((() => { try { return localStorage.getItem(OPEN_KEY) !== '0'; } catch { return true; } })());
effect(() => { try { localStorage.setItem(OPEN_KEY, checksOpen.value ? '1' : '0'); } catch { /* not remembered */ } });

const fw = fwVersion;
const hex32 = (n: number): string => `0x${(n >>> 0).toString(16).padStart(8, '0')}`;
const num = (v: number) => v.toLocaleString(lang.value);

function uptime(ms: number): string {
  const s = Math.floor(ms / 1000);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
  return h ? `${h} h ${String(m).padStart(2, '0')} min` : m ? `${m} min ${String(s % 60).padStart(2, '0')} s` : `${s} s`;
}

// --- Connection ------------------------------------------------------------------

function ConnectionPanel() {
  const d = device.value;
  const [error, setError] = useState<string | null>(null);
  const [checking, setChecking] = useState(false);
  const connected = d.state === 'connected';
  const run = (fn: () => Promise<unknown>) => () => {
    setError(null);
    fn().catch((err: unknown) => {
      const msg = err instanceof Error ? err.message : String(err);
      // Closing the browser's picker is not an error.
      if (!/no port selected|cancelled|canceled|User cancel/i.test(msg)) setError(msg);
    });
  };
  const dot = d.state === 'connected' ? d.report?.overall ?? 'ok' : d.state === 'disconnected' ? '' : 'warn';
  const stateText = d.state === 'connected' ? `${t(`conn.${d.link}` as Key)}${d.label && d.link !== 'sim' ? ' · ' + d.label : ''}`
    : t(`st.state.${d.state}` as Key);
  const st = d.status;
  const powered = st?.lanesPowered === 1;
  const ctrl = d.nodes.ctrl;
  const rows: [Key, string][] = connected ? [
    ['st.protocol', d.hello ? `${d.hello.protoMajor}.${d.hello.protoMinor}` : '–'],
    ['st.boot', d.hello ? hex32(d.hello.bootId) : '–'],
    ['st.uptime', st ? uptime(st.uptimeMs) : '–'],
    ['st.rtt', (() => { const r = rttMs(); return r === null ? '–' : `${r.toFixed(1)} ms`; })()],
    ['st.fwControl', fw(ctrl)],
    ['st.fwBle', fw(d.nodes.ble)],
  ] : [];

  return html`<${Panel} title=${t('status.connection')} class="st-conn">
    <div class="conn-row">
      <span class=${'conn-dot ' + dot}></span>
      <span>${stateText}</span>
      ${connected && d.report && html`<span class=${'tag st-tag-' + d.report.overall}>${t(`st.overall.${d.report.overall}` as Key)}</span>`}
    </div>
    ${d.state === 'unbound' && html`<p class="hint">${t('st.unboundHint')}</p>`}
    ${d.state === 'disconnected' && d.mode === 'device' && html`<p class="hint">${t('st.waitingHint')}</p>`}
    ${d.reason && d.state !== 'connected' && html`<p class="field-warn">${d.reason}</p>`}
    ${error && html`<p class="field-warn">${error}</p>`}

    ${connected && html`<dl class="st-facts">
      ${rows.map(([k, v]) => html`<div><dt>${t(k)}</dt><dd class="tnum">${v}</dd></div>`)}
    </dl>`}

    <div class="st-actions">
      ${d.mode === 'off' || d.state === 'disconnected' ? html`
        <${Button} kind="primary" disabled=${!canUsb} onClick=${run(connectUsb)}
          title=${canUsb ? undefined : t('st.noSerial')}>${t('st.connectUsb')}<//>
        <${Button} disabled=${!canBle} onClick=${run(connectBle)}
          title=${canBle ? undefined : t('st.noBle')}>${t('st.connectBle')}<//>
        <${Button} onClick=${run(connectSim)}>${t('st.connectSim')}<//>`
      : html`
        <${Button} disabled=${!connected || checking}
          onClick=${() => { setChecking(true); run(() => selfCheck().finally(() => setChecking(false)))(); }}>
          ${checking ? t('st.checking') : t('st.selfCheck')}<//>
        <${LanePowerSwitch} />`}
      ${d.mode !== 'off' && html`<${Button} onClick=${run(disconnect)}>${t('st.disconnect')}<//>`}
    </div>
    ${connected && !powered && st && html`<p class="hint">${t('st.lanePowerHint')}</p>`}
  <//>`;
}

let simSensorFault = false;

/**
 * Lane power as an on/off switch. It shows the wanted state at once; the rail
 * then takes a moment to settle (STATUS.lanesPowered follows).
 */
function LanePowerSwitch() {
  const st = device.value.status;
  const connected = device.value.state === 'connected';
  const powered = st?.lanesPowered === 1;
  const [want, setWant] = useState<boolean | null>(null);
  useEffect(() => {
    if (want === null) return;
    if (want === powered) { setWant(null); return; }
    const id = setTimeout(() => setWant(null), 5000);   // gave up: show what the device says
    return () => clearTimeout(id);
  }, [want, powered]);
  const on = want ?? powered;
  const disabled = !connected || !st;
  return html`<label class=${'switch st-power' + (disabled ? ' disabled' : '') + (want !== null ? ' pending' : '')}>
    <span>${t('st.lanePower')}</span>
    <input type="checkbox" role="switch" checked=${on} disabled=${disabled}
      onChange=${(e: Event) => {
        const v = (e.target as HTMLInputElement).checked;
        setWant(v);
        lanePower(v).catch(() => setWant(null));
      }} />
    <span class="switch-track"></span>
    <span class="st-power-state">${on ? t('st.on') : t('st.off')}</span>
  </label>`;
}

/** Simulator knobs (dev tools on, simulator connected): to see the checks react. */
function SimPanel() {
  const [, force] = useState(0);
  const s = sim();
  const d = device.value;
  if (!s || !devTools.value || d.state !== 'connected') return null;
  const act = (fn: () => void) => () => { fn(); force(x => x + 1); };
  return html`<${Panel} title=${t('st.sim')} tag=${t('set.devTools')}>
    <div class="st-actions">
      <${Button} onClick=${act(() => s.setCars(!s.cars))}>${s.cars ? t('st.sim.carsOff') : t('st.sim.carsOn')}<//>
      <${Button} onClick=${act(() => s.dropNextEvents(3))}>${t('st.sim.drop')}<//>
      <${Button} onClick=${act(() => s.nrfAlive(s.bleBridge === BleBridgeState.ABSENT))}>
        ${s.bleBridge === BleBridgeState.ABSENT ? t('st.sim.nrfOn') : t('st.sim.nrfOff')}<//>
      <${Button} onClick=${act(() => { simSensorFault = !simSensorFault; s.sensorFault(1, simSensorFault); })}>
        ${simSensorFault ? t('st.sim.sensorOk') : t('st.sim.sensorFault')}<//>
      <${Button} onClick=${act(() => s.powerCycle())}>${t('st.sim.powerCycle')}<//>
    </div>
  <//>`;
}

// --- Health cards ------------------------------------------------------------------

function CheckRow({ c }: { c: Check }) {
  return html`<li class=${'st-check lv-' + c.level} title=${DESCRIPTION.get(c.id)}>
    <span class=${'conn-dot ' + (c.level === 'unknown' ? '' : c.level)}></span>
    <span class="st-check-title">${t(`chk.${c.id}` as Key)}</span>
    <span class="st-check-msg">${c.message}</span>
  </li>`;
}

function PartCard({ title, sub, online, checks }:
  { title: string; sub?: string; online: boolean; checks: readonly Check[] }) {
  const lvl: CheckLevel = online ? worst(checks.map(c => c.level)) : 'unknown';
  return html`<section class=${'panel st-card' + (online ? '' : ' offline')}>
    <h2 class="panel-title st-card-title">
      <span>${title}</span>
      ${sub && html`<em class="tag">${sub}</em>`}
      <i class=${'conn-dot st-card-dot ' + (lvl === 'unknown' ? '' : lvl)}></i>
    </h2>
    <ul class="st-checks">${checks.map(c => html`<${CheckRow} key=${c.id} c=${c} />`)}</ul>
  </section>`;
}

function HealthCards() {
  const d = device.value;
  const r = d.report;
  if (!r) return null;
  const bleSub = r.ble.connected ? t('st.bleHost') : r.ble.online ? t('st.bleNoHost') : undefined;
  return html`<div class="st-grid">
    <${PartCard} title=${t('st.part.control')} sub=${fw(d.nodes.ctrl)} online=${r.control.online || d.state === 'connected'} checks=${r.control.checks} />
    <${PartCard} title=${t('st.part.ble')} sub=${bleSub} online=${r.ble.online} checks=${r.ble.checks} />
  </div>`;
}

// --- Sensors -----------------------------------------------------------------------

const W = 600, H = 150, PAD = 4;

/** Raw signal with its min/max band, baseline, thresholds, reference and markers over the last 60 s. */
function LaneGraph({ samples, markers, now }: { samples: LaneSample[]; markers: LaneMarker[]; now: number }) {
  const span = MONITOR_DEFAULTS.historyMs;
  const t0 = now - span;
  if (!samples.length) return html`<div class="st-graph empty">${t('st.noSamples')}</div>`;
  let lo = Infinity, hi = -Infinity;
  for (const s of samples) {
    lo = Math.min(lo, s.rawMin, s.thrLow);
    hi = Math.max(hi, s.rawMax, s.baseline, s.thrHigh, s.reference || s.baseline);
  }
  const m = Math.max(20, (hi - lo) * 0.08);
  lo -= m; hi += m;
  const x = (tMs: number) => ((tMs - t0) / span) * W;
  const y = (v: number) => PAD + (1 - (v - lo) / (hi - lo)) * (H - 2 * PAD);
  const line = (f: (s: LaneSample) => number) => samples.map(s => `${x(s.tMs).toFixed(1)},${y(f(s)).toFixed(1)}`).join(' ');
  const band = [...samples.map(s => `${x(s.tMs).toFixed(1)},${y(s.rawMax).toFixed(1)}`),
    ...samples.slice().reverse().map(s => `${x(s.tMs).toFixed(1)},${y(s.rawMin).toFixed(1)}`)].join(' ');
  const hasRef = samples.some(s => s.reference > 0);
  return html`<svg class="st-graph" viewBox=${`0 0 ${W} ${H}`} preserveAspectRatio="none" role="img"
      aria-label=${t('st.graphLabel')}>
    <polygon class="g-band" points=${band} />
    <polyline class="g-thr" points=${line(s => s.thrHigh)} />
    <polyline class="g-thr" points=${line(s => s.thrLow)} />
    ${hasRef && html`<polyline class="g-ref" points=${line(s => s.reference)} />`}
    <polyline class="g-base" points=${line(s => s.baseline)} />
    <polyline class="g-raw" points=${line(s => s.raw)} />
    ${markers.filter(mk => mk.tMs >= t0).map(mk => html`<line class=${'g-mark g-mark-' + mk.kind}
        x1=${x(mk.tMs)} x2=${x(mk.tMs)} y1="0" y2=${H}><title>${mk.label}</title></line>`)}
  </svg>`;
}

function outcomeText(o: CalibrationOutcome): string {
  const r = t(`st.cal.${o.result}` as Key);
  return o.result === 'saved' || o.result === 'notSaved' ? `${r} (${num(o.reference)})` : o.timedOut ? `${r} (${t('st.cal.timeout')})` : r;
}

/** One lane: its live values and graph on top, its checks below. */
function LaneCard({ lane, online, checks, info, onCalibrate, calibrating, canCalibrate, result }:
  { lane: number; online: boolean; checks: readonly Check[]; info: NodeInfo | null | undefined;
    onCalibrate: () => void; calibrating: boolean; canCalibrate: boolean; result?: string }) {
  const mon = monitor();
  const lvl: CheckLevel = online ? worst(checks.map(c => c.level)) : 'unknown';
  const ls = online ? mon?.lastLaneStatus(lane) ?? null : null;
  const vals: [string, number | string][] = ls ? [
    [t('st.v.raw'), ls.raw], [t('st.v.baseline'), ls.baseline], [t('st.v.thr'), `${num(ls.thrLow)} / ${num(ls.thrHigh)}`],
    [t('st.v.noise'), ls.noise], [t('st.v.reference'), ls.reference || '–'], [t('st.v.saved'), ls.savedReference || '–'],
  ] : [];
  const open = checksOpen.value;
  const shownChecks = online ? checks : checks.slice(0, 1);
  const fails = shownChecks.filter(c => c.level === 'fail').length;
  const warns = shownChecks.filter(c => c.level === 'warn').length;
  const summary = !online ? checks[0]?.message ?? ''
    : fails || warns ? [fails && t('st.sum.fail', { n: fails }), warns && t('st.sum.warn', { n: warns })].filter(Boolean).join(' · ')
    : t('st.sum.ok');
  return html`<section class=${'panel st-card st-lane' + (online ? '' : ' offline') + (open ? ' open' : '')} style=${`--lane: var(--lane-${lane})`}>
    <h2 class="panel-title st-card-title">
      <span>${t('st.part.lane', { n: lane })}</span>
      ${online && html`<em class="tag">${fw(info)}</em>`}
      ${!open && html`<small class=${'st-card-sum lv-' + lvl}>${summary}</small>`}
      <i class=${'conn-dot st-card-dot ' + (lvl === 'unknown' ? '' : lvl)}></i>
    </h2>
    ${online && mon && html`<div class="st-lane-live">
      <div class="st-lane-head">
        <dl class="st-vals">${vals.map(([k, v]) => html`<div><dt>${k}</dt><dd class="tnum">${typeof v === 'number' ? num(v) : v}</dd></div>`)}</dl>
        <${Button} disabled=${!canCalibrate} onClick=${onCalibrate}>${calibrating ? t('st.cal.running') : t('st.cal.one')}<//>
      </div>
      <${LaneGraph} samples=${mon.samples(lane)} markers=${mon.markers(lane)} now=${realScheduler.now()} />
      ${result && html`<p class="hint st-cal-result">${t('st.cal.last')}: ${result}</p>`}
    </div>`}
    ${open && html`<ul class="st-checks">${shownChecks.map(c => html`<${CheckRow} key=${c.id} c=${c} />`)}</ul>`}
  </section>`;
}

/** The lanes, two per row (1 2 / 3 4 / 5 6), each with its sensor readout and checks; calibration on top. */
function LanesPanel() {
  void samplesTick.value;                 // re-render with new samples
  const d = device.value;
  const mon = monitor();
  const [confirm, setConfirm] = useState<{ lanes: number[] | 'all'; forget: boolean } | null>(null);
  const [busyLanes, setBusyLanes] = useState<readonly number[] | 'all' | null>(null);
  const [results, setResults] = useState<Record<number, string>>({});
  const [error, setError] = useState<string | null>(null);
  const r = d.report;
  if (!r) return null;
  const connected = d.state === 'connected' && !!mon;
  const present = connected ? presentLanes(d.nodes) : [];
  const shown = Math.max(trackLanes.value, ...present, ...r.lanes.filter(l => l.checks.some(c => c.level !== 'unknown')).map(l => l.index));
  const raceBusy = mon?.busy() ?? false;

  const go = async (sel: number[] | 'all', forget: boolean) => {
    setError(null);
    setBusyLanes(sel);
    const out = await calibrate(sel, forget);
    setBusyLanes(null);
    if (out === 'busy') { setError(t('st.cal.busy')); return; }
    if (out === 'failed') { setError(t('st.cal.error')); return; }
    setResults(r => ({ ...r, ...Object.fromEntries(out.map(o => [o.lane, outcomeText(o)])) }));
  };
  const working = busyLanes !== null;
  const isBusy = (lane: number) => busyLanes === 'all' || (Array.isArray(busyLanes) && busyLanes.includes(lane));

  const canAll = connected && present.length > 0 && !working && !raceBusy;
  return html`<${Panel} title=${t('st.lanes')} class="st-sensors">
    <div class="st-actions st-sensor-head">
      <p class="hint">${!connected ? t('st.lanesOffline') : raceBusy ? t('st.sensorsBusy')
        : present.length ? t('st.sensorsHint', { hz: mon!.laneRateHz() }) : t('st.noLanes')}</p>
      <${Button} onClick=${() => { checksOpen.value = !checksOpen.value; }}>${checksOpen.value ? t('st.collapseAll') : t('st.expandAll')}<//>
      <${Button} disabled=${!canAll} onClick=${() => setConfirm({ lanes: 'all', forget: false })}>${t('st.cal.all')}<//>
      <${Button} kind="danger" disabled=${!canAll} onClick=${() => setConfirm({ lanes: 'all', forget: true })}>${t('st.cal.forgetAll')}<//>
    </div>
    ${error && html`<p class="field-warn">${error}</p>`}
    <div class="st-lanes">
      ${r.lanes.filter(l => l.index <= shown).map(l => html`<${LaneCard} key=${l.index} lane=${l.index}
        online=${connected && l.online} checks=${l.checks} info=${d.nodes.lanes[l.index]?.info}
        calibrating=${isBusy(l.index)} canCalibrate=${!working && !raceBusy} result=${results[l.index]}
        onCalibrate=${() => setConfirm({ lanes: [l.index], forget: false })} />`)}
    </div>
    ${present.length > 0 && html`<ul class="st-legend">
      <li class="lg-raw">${t('st.lg.raw')}</li><li class="lg-base">${t('st.lg.baseline')}</li>
      <li class="lg-thr">${t('st.lg.thr')}</li><li class="lg-ref">${t('st.lg.reference')}</li><li class="lg-mark">${t('st.lg.marker')}</li>
    </ul>`}
    <${ConfirmDialog} open=${confirm !== null}
      title=${confirm?.forget ? t('st.cal.forgetTitle') : t('st.cal.title')}
      body=${confirm?.forget ? t('st.cal.forgetBody') : t('st.cal.body')}
      confirmLabel=${confirm?.forget ? t('st.cal.forgetConfirm') : t('st.cal.confirm')} cancelLabel=${t('common.cancel')}
      danger=${confirm?.forget ?? false}
      onConfirm=${() => { if (confirm) void go(confirm.lanes, confirm.forget); }}
      onClose=${() => setConfirm(null)} />
  <//>`;
}

// --- Event trace -------------------------------------------------------------------

function Trace() {
  const items = trace.value;
  const time = (at: number) => new Date(at).toLocaleTimeString(lang.value, { hour12: false }) + '.' + String(at % 1000).padStart(3, '0');
  return html`<details class="panel st-trace">
    <summary class="panel-title"><span>${t('st.trace')}</span><em class="tag">${items.length}</em></summary>
    <div class="panel-body">
      ${!items.length ? html`<p class="hint">${t('st.traceEmpty')}</p>` : html`<div class="st-trace-scroll"><table class="st-trace-table">
        <thead><tr><th>${t('st.tr.time')}</th><th>evSeq</th><th>${t('st.tr.event')}</th><th>${t('st.tr.details')}</th></tr></thead>
        <tbody>${items.map(e => html`<tr key=${e.key} class=${e.kind}>
          <td class="tnum">${time(e.at)}</td><td class="tnum">${e.evSeq ?? ''}</td><td>${e.name}</td><td>${e.text}</td>
        </tr>`)}</tbody>
      </table></div>`}
    </div>
  </details>`;
}

export function StatusPage() {
  const a = AREAS.find(x => x.id === 'status')!;
  return html`<${PageHeader} title=${t(a.title)} subtitle=${t(a.subtitle)}>
      <a class="btn btn-ghost" href="../debug/"><span>${t('status.debugLink')}</span></a>
    <//>
    <${ConnectionPanel} />
    <${SimPanel} />
    <${HealthCards} />
    <${LanesPanel} />
    <${Trace} />`;
}
