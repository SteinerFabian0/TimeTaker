// Track Setup (GUI_SPEC §8). Two parts:
//   Race timing   start lights and lap timing: per track, used by every race
//                 whatever preset it runs. Works without the Power Pack.
//   Track power   speed level and (expert) power limits: needs the Power Pack.
import { useEffect, useRef, useState } from 'preact/hooks';
import { html } from '../lib/html.ts';
import { t, type Key } from '../i18n/i18n.ts';
import { trackTiming, updateTrackTiming, resetTrackTiming } from '../lib/trackTiming.ts';
import { trackLanes, setTrackLanes } from '../lib/trackLanes.ts';
import { LANE_OPTIONS, allowedLanes, type LaneCount } from '../../src/track/lanes.ts';
import { TIMING_LIMITS, isDefaultTrackTiming, type TrackTiming } from '../../src/track/trackTiming.ts';
import { AREAS } from '../shell/areas.ts';
import { avgLapMs, changeoverMs, ESTIMATE_LIMITS } from '../lib/prefs.ts';
import { connection, trackLink, raceSessionActive } from '../shell/connection.ts';
import { planStartLights, startLightLevels, type StartLightsPlan } from '../../src/track/startLightsPlan.ts';
import type { StartLightsConfig } from '../../src/race/types.ts';
import { PageHeader, Panel, Button, Segmented, Field, Stepper, PowerPackLock, ConfirmDialog } from '../ui/ui.ts';

type LightKey = keyof TrackTiming['startLights'];

/** One start-light field. Times are stored in ms and shown in seconds. */
function LightField({ k, label, hint, seconds = true, min, max, children }:
  { k: LightKey; label: Key; hint?: Key; seconds?: boolean; min: number; max: number; children?: unknown }) {
  const sl = trackTiming.value.startLights;
  const scale = seconds ? 1000 : 1;
  const id = 'ts-sl-' + k;
  const stepper = html`<${Stepper} id=${id} label=${t(label)} value=${sl[k] / scale} unit=${seconds ? 's' : undefined}
      min=${min / scale} max=${max / scale} step=${seconds ? 0.1 : 1} decimals=${seconds ? 1 : 0}
      onChange=${(v: number) => updateTrackTiming({ startLights: { ...sl, [k]: Math.round(v * scale) } })} />`;
  return html`<${Field} label=${t(label)} id=${id} hint=${hint && t(hint)}>
    ${children ? html`<div class="field-with-action">${stepper}${children}</div>` : stepper}
  <//>`;
}


/** Lead before T0 so the track and the page start together (device minimum 150 ms). */
const PREVIEW_LEAD_MS = 200;
/** How long "Go!" stays after lights out. */
const PREVIEW_GO_MS = 1500;

type HoldKind = 'min' | 'max';
interface PreviewRun { sl: StartLightsConfig; plan: StartLightsPlan; t0: number; onTrack: boolean; kind: HoldKind }

/**
 * The start light preview: plays the sequence with the shortest or longest
 * hold, here and on the track when a time taker is connected and idle (no race
 * session, no sequence running). One light per lane of the connected time
 * taker; one per lane of the track (Track → Lanes), which never exceeds them.
 */
function useLightsPreview() {
  const link = trackLink.value;
  const busy = !!link && (raceSessionActive.value || link.seqRunning);
  const lanes = trackLanes.value;
  const [run, setRun] = useState<PreviewRun | null>(null);
  const [now, setNow] = useState(0);
  const [failed, setFailed] = useState(false);
  const raf = useRef(0);

  useEffect(() => {
    if (!run) return;
    const tick = () => {
      const t = performance.now();
      if (t - run.t0 > run.plan.outMs + PREVIEW_GO_MS) { setRun(null); return; }
      setNow(t);
      raf.current = requestAnimationFrame(tick);
    };
    raf.current = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf.current);
  }, [run]);

  const start = (kind: HoldKind) => {
    const sl = trackTiming.value.startLights;
    const hold = kind === 'min' ? sl.holdMinMs : sl.holdMaxMs;
    const plan = planStartLights(sl, lanes, hold);
    const onTrack = !!link && !busy;
    setFailed(false);
    if (onTrack) {
      link.api.startSequence({
        lights: plan.lanes, stepMs: sl.stepMs, holdMinMs: hold, holdMaxMs: hold, armSampling: false,
        leadMs: PREVIEW_LEAD_MS, pulseRiseMs: sl.pulseRiseMs, attentionPulses: sl.attentionPulses,
      }).catch(() => setFailed(true));
    }
    const t0 = performance.now() + PREVIEW_LEAD_MS;
    setNow(performance.now());
    setRun({ sl, plan, t0, onTrack, kind });
  };
  const stop = () => {
    if (run?.onTrack && !failed) void trackLink.value?.api.abortSequence().catch(() => {});
    setRun(null);
  };

  const ms = run ? now - run.t0 : -1;
  const levels: number[] = run ? startLightLevels(run.sl, run.plan, ms) : new Array(lanes).fill(0);
  const go = !!run && ms >= run.plan.outMs;
  const note: Key = failed ? 'ts.lights.previewFailed' : !link ? 'ts.lights.previewNoTrack'
    : busy ? 'ts.lights.previewBusy' : 'ts.lights.previewTrack';
  return { running: run?.kind ?? null, start, stop, levels, go, note };
}

type Preview = ReturnType<typeof useLightsPreview>;

/** Preview (or Stop) next to a hold field. */
function PreviewButton({ p, kind }: { p: Preview; kind: HoldKind }) {
  if (p.running === kind) return html`<${Button} onClick=${p.stop}>${t('ts.lights.previewStop')}<//>`;
  return html`<${Button} kind="primary" disabled=${p.running !== null} onClick=${() => p.start(kind)}
    title=${t(kind === 'min' ? 'ts.lights.previewMin' : 'ts.lights.previewMax')}>${t('ts.lights.preview')}<//>`;
}

/** One red light per lane, lane 1 (start of the chain) on the left. */
function LightsRow({ p }: { p: Preview }) {
  return html`<div class="lights-preview">
    <div class="lights-row" role="img" aria-label=${t('ts.lights.previewLabel')}>
      ${p.levels.map((lv, i) => html`<div class="light-cell">
        <span class="light" style=${'--lvl:' + lv.toFixed(3)}></span>
        <span class="light-lane">${i + 1}</span></div>`)}
      <span class=${'lights-go' + (p.go ? ' on' : '')} aria-live="polite">${p.go ? t('ts.lights.go') : ''}</span>
    </div>
    <p class="hint">${t(p.note)}</p>
  </div>`;
}

function RaceTiming() {
  const tt = trackTiming.value;
  const sl = tt.startLights;
  const L = TIMING_LIMITS;
  const [confirmReset, setConfirmReset] = useState(false);
  const preview = useLightsPreview();
  return html`<section class="ts-section" aria-labelledby="ts-timing">
    <div class="ts-section-head">
      <h2 id="ts-timing" class="section-title">${t('ts.timing')}</h2>
      <${Button} disabled=${isDefaultTrackTiming(tt)} onClick=${() => setConfirmReset(true)}>${t('ts.timing.reset')}<//>
    </div>
    <p class="hint ts-section-hint">${t('ts.timing.hint')}</p>
    <div class="ts-timing-grid">
      <${Panel} title=${t('ts.lights')}>
        <div class="ts-lights-groups">
          <div>
            <h3 class="sub-title first">${t('ts.lights.attention')}</h3>
            <div class="field-row">
              <${LightField} k="attentionPulses" label="ts.lights.pulses" seconds=${false} min=${L.attentionPulses.min} max=${L.attentionPulses.max} />
              <${LightField} k="pulseRiseMs" label="ts.lights.rise" min=${L.pulseRiseMs.min} max=${L.pulseRiseMs.max} />
            </div>
          </div>
          <div>
            <h3 class="sub-title first">${t('ts.lights.countdown')}</h3>
            <div class="field-row">
              <${LightField} k="stepMs" label="ts.lights.step" min=${L.stepMs.min} max=${L.stepMs.max} />
            </div>
          </div>
        </div>
        <h3 class="sub-title">${t('ts.lights.hold')}</h3>
        <div class="field-row ts-hold-row">
          <${LightField} k="holdMinMs" label="ts.lights.holdMin" min=${L.holdMs.min} max=${sl.holdMaxMs}>
            <${PreviewButton} p=${preview} kind="min" /><//>
          <${LightField} k="holdMaxMs" label="ts.lights.holdMax" min=${sl.holdMinMs} max=${L.holdMs.max}>
            <${PreviewButton} p=${preview} kind="max" /><//>
        </div>
        <p class="hint">${t('ts.lights.hint')}</p>
        <${LightsRow} p=${preview} />
      <//>
      <${Panel} title=${t('ts.lapTiming')}>
        <${Field} label=${t('ts.minLap')} id="ts-minlap" hint=${t('ts.minLap.hint')}>
          <${Stepper} id="ts-minlap" label=${t('ts.minLap')} value=${tt.minLapMs / 1000} unit="s"
            min=${L.minLapMs.min / 1000} max=${L.minLapMs.max / 1000}
            onChange=${(s: number) => updateTrackTiming({ minLapMs: Math.round(s * 1000) })} />
        <//>
        <h3 class="sub-title">${t('set.estimates')}</h3>
        <div class="field-row">
          <${Field} label=${t('set.avgLap')} id="ts-avg">
            <${Stepper} id="ts-avg" label=${t('set.avgLap')} unit="s" value=${avgLapMs.value / 1000}
              min=${ESTIMATE_LIMITS.avgLapMs.min / 1000} max=${ESTIMATE_LIMITS.avgLapMs.max / 1000}
              onChange=${(v: number) => { avgLapMs.value = Math.round(v * 1000); }} />
          <//>
          <${Field} label=${t('set.changeover')} id="ts-change">
            <${Stepper} id="ts-change" label=${t('set.changeover')} unit="s" value=${changeoverMs.value / 1000}
              min=${ESTIMATE_LIMITS.changeoverMs.min / 1000} max=${ESTIMATE_LIMITS.changeoverMs.max / 1000} step=${5}
              onChange=${(v: number) => { changeoverMs.value = Math.round(v * 1000); }} />
          <//>
        </div>
        <p class="hint">${t('set.estimatesHint')}</p>
      <//>
    </div>
    <${ConfirmDialog} open=${confirmReset} title=${t('ts.timing.resetTitle')} body=${t('ts.timing.resetBody')}
      confirmLabel=${t('ts.timing.reset')} cancelLabel=${t('common.cancel')}
      onConfirm=${resetTrackTiming} onClose=${() => setConfirmReset(false)} />
  </section>`;
}

/** The track itself: how many lanes it has (GUI_SPEC §8.0). */
function TrackLanes() {
  const reported = trackLink.value?.lanes ?? null;
  const ok = allowedLanes(reported);
  return html`<section class="ts-section" aria-labelledby="ts-track">
    <div class="ts-section-head"><h2 id="ts-track" class="section-title">${t('ts.track')}</h2></div>
    <${Panel} title=${t('ts.lanes')}>
      <${Field} label=${t('ts.lanes.label')}
          hint=${reported ? t('ts.lanes.hintDevice', { n: reported }) : t('ts.lanes.hint')}>
        <${Segmented} big label=${t('ts.lanes.label')} value=${String(trackLanes.value)}
          onChange=${(v: string) => setTrackLanes(Number(v) as LaneCount)}
          options=${LANE_OPTIONS.map(n => ({
            value: String(n), label: String(n), disabled: !ok.includes(n),
            title: ok.includes(n) ? undefined : t('ts.lanes.locked', { n: reported ?? 0 }),
          }))} />
      <//>
    <//>
  </section>`;
}

/** Expert settings (power limits, per-lane limits, compensation) come here with the Power Pack. */
function TrackPower() {
  const pp = connection.value.powerPack;
  const [level, setLevel] = useState<string>('3');
  return html`<section class="ts-section" aria-labelledby="ts-power">
    <div class="ts-section-head"><h2 id="ts-power" class="section-title">${t('ts.power')}</h2></div>
    ${!pp && html`<${PowerPackLock} />`}
    <${Panel} title=${t('ts.speed')} class=${pp ? '' : 'locked'}>
      <${Segmented} big label=${t('ts.speed')} disabled=${!pp} value=${level} onChange=${setLevel}
        options=${[1, 2, 3, 4, 5].map(n => ({ value: String(n), label: String(n) }))} />
      <p class="hint">${t('ts.speedHint')}</p>
    <//>
  </section>`;
}

export function TrackSetupPage() {
  const a = AREAS.find(x => x.id === 'track-setup')!;
  const pp = connection.value.powerPack;
  // Expert mode is local state: off every time the page is opened (GUI_SPEC §8.2).
  const [expert, setExpert] = useState(false);
  return html`
    <${PageHeader} title=${t(a.title)} subtitle=${t(a.subtitle)}>
      <label class=${'switch' + (pp ? '' : ' disabled')}>
        <input type="checkbox" checked=${expert} disabled=${!pp}
          onChange=${(e: Event) => setExpert((e.target as HTMLInputElement).checked)} />
        <span class="switch-track"></span><span>${t('ts.expert')}</span></label>
    <//>
    <p class="hint expert-hint">${t('ts.expertHint')}</p>
    <${TrackLanes} />
    <${RaceTiming} />
    <${TrackPower} />`;
}
