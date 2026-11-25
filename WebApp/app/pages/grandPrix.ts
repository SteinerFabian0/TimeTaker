// Grand Prix (GUI_SPEC §6): the evening as one forward movement. A small
// timeline at the top: tournament → drivers → cars → overview → one tab per phase
// of the tree (groups, second chance, finals) → podium. The setup pages end
// with "Next"; "Start Grand Prix" on the overview opens the first phase: its
// settings page (race preset, laps or minutes, races per lane, taking turns; the template's
// values are the default, changes are marked), then its race page (who sits
// on which lane; the host can move the empty lane, and the time taker lights
// the used lanes). Racing itself comes later.
// Rules: src/tournaments/plan.ts, src/grandPrix/{names,phase}.ts, src/presets/schedule.ts.
import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'preact/hooks';
import { html } from '../lib/html.ts';
import { t, type Key } from '../i18n/i18n.ts';
import { presets, stagePresets, presetName } from '../lib/presets.ts';
import { trackLanes } from '../lib/trackLanes.ts';
import { templates, templatesLoaded, templateName } from '../lib/tournaments.ts';
import {
  gpStep, gpTemplateId, gpDrivers, gpDraw, gpFocusId, addDriver, renameDriver, removeDriver, moveDriver, drawGroups,
  gpFinalSeats, gpStarted, gpCars, carOf, setCar, applyCarSet, laneColorVars, gpRound, gpView, gpSection, gpPhases, gpSeating, gpLocked, gpDone, raceKey, resetEvening,
  type GpStep, type GpDriver,
} from '../lib/grandPrix.ts';
import { wantedLaneLights, laneLightsOk } from '../lib/laneLights.ts';
import { beginWarmup, warmupOf } from '../lib/warmup.ts';
import { abortStart, beginRace, raceAborted, raceOf, raceStartFailed, resetToPreRace, simulateCrossing } from '../lib/race.ts';
import { trackTiming } from '../lib/trackTiming.ts';
import { fold } from '../../src/race/raceEngine.ts';
import { resolveConfig } from '../../src/race/rules/config.ts';
import type { CarResult, RaceState } from '../../src/race/types.ts';
import { planStartLights, startLightLevels } from '../../src/track/startLightsPlan.ts';
import { finalLapLanes, formatLap, lapClock, lapOn, LAST_LAP_HOLD_MS } from '../../src/grandPrix/raceClock.ts';
import { carDrive, laneWarmup, stopLanes, warmupDriveMs, type CarDrive, type LaneWarmup } from '../../src/grandPrix/warmup.ts';
import { trackLink } from '../shell/connection.ts';
import { LIMITS, type TournamentTemplate } from '../../src/tournaments/template.ts';
import type { Stage } from '../../src/tournaments/plan.ts';
import { sameCars, type CarSetup } from '../../src/grandPrix/cars.ts';
import { Car, CarBoard, CarChips } from '../ui/carBoard.ts';
import { carPresets, createCarPreset } from '../lib/carPresets.ts';
import { typedName, keepName, shortCodes, NAME_MAX } from '../../src/grandPrix/names.ts';
import { applyPhase, changedFields, choosePreset, phasesOf, setPhaseField, DEFAULT_SECTIONS, type Phase, type PhaseKind } from '../../src/grandPrix/phase.ts';
import { defaultSeating, inactiveLanes, moveToLane, raceScheduleSeated, validSeating } from '../../src/presets/schedule.ts';
import { presetOption } from './racePresets.ts';
import { racesText, sizesText, problemText, plannedFor, stageName, ordinal, Tree, type Planned } from './tournaments.ts';
import { runOrder, SECTION_CHOICES } from '../../src/tournaments/runOrder.ts';
import { isQualifying, racesEstimateMs, warmupBefore, toRaceConfig, BUILTIN_IDS, LIMITS as PRESET_LIMITS, type RacePreset } from '../../src/presets/preset.ts';
import { avgLapMs, changeoverMs, devTools } from '../lib/prefs.ts';
import { useSortable } from '../ui/sortable.ts';
import { Button, Segmented, Field, Select, Stepper, formatDuration } from '../ui/ui.ts';

// --- shared --------------------------------------------------------------------------

const named = (list: readonly GpDriver[]) => list.filter(d => d.name);

function chosenTemplate(): TournamentTemplate | null {
  return templates.value.find(x => x.id === gpTemplateId.value) ?? null;
}

/**
 * Tonight's plan: the template for these drivers with the evening's choices:
 * the small final as the template says, the final's size (▲ / ▼; a size that
 * no longer works falls back to the lanes) and each phase's race settings.
 */
function eveningPlan(x: TournamentTemplate, n: number) {
  const lanes = trackLanes.value;
  const small = x.smallFinal;
  const adjust = (s: Stage) => applyPhase(basePreset(s), gpPhases.value[String(s.column)]);
  const works = (f: number) => f >= 2 && f <= lanes
    && !plannedFor(x, n, { smallFinal: small, finalSeats: f }).plan.problems.some(p => p.level === 'error');
  const F = small && gpFinalSeats.value !== null && works(gpFinalSeats.value) ? gpFinalSeats.value : lanes;
  const pl = plannedFor(x, n, { smallFinal: small, finalSeats: F }, adjust);
  const K = pl.plan.stages.find(s => s.kind === 'smallFinal');
  const setF = (f: number) => { gpFinalSeats.value = f === lanes ? null : f; };
  // ▼ moves the final's last seat into the small final (while it has room), ▲ brings one back.
  const down = K && K.seats.length < lanes && works(F - 1) ? () => setF(F - 1) : null;
  const up = K && F < lanes && works(F + 1) ? () => setF(F + 1) : null;
  return { pl, up, down, hasSmall: !!K };
}

const presetById = (id: string | undefined) => presets.value.find(p => p.id === id);

/** The template's own preset of a stage. */
function templatePreset(s: Stage): RacePreset {
  return presetById(s.presetId) ?? presetById(BUILTIN_IDS.default)!;
}

/** The preset a stage's phase races tonight, before its own changes: the host's pick, else the template's. */
function basePreset(s: Stage): RacePreset {
  return presetById(gpPhases.value[String(s.column)]?.presetId) ?? templatePreset(s);
}

/**
 * Picks the race preset of a stage's phase (every stage of a phase races the
 * same, so group A's dropdown also changes group B's). The tournament's
 * preset again clears the pick.
 */
function pickPreset(s: Stage, id: string): void {
  const col = String(s.column);
  gpPhases.value = { ...gpPhases.value, [col]: choosePreset(gpPhases.value[col], templatePreset(s).id, id) };
}

/**
 * The race preset dropdown of a stage: blue while it differs from the
 * template's; in the overview cards a ↺ beside it goes back (the phase
 * settings page has its "Back to the template" line instead).
 */
function PresetChoice({ s, big }: { s: Stage; big?: boolean }) {
  const tpl = templatePreset(s);
  const cur = basePreset(s);
  const changed = cur.id !== tpl.id;
  const list = stagePresets.value;
  const options = [...(list.some(p => p.id === cur.id) ? [] : [cur]), ...list].map(presetOption);
  const reset = t('gp.preset.reset', { name: presetName(tpl) });
  return html`<div class=${'gp-preset' + (changed ? ' changed' : '') + (big ? ' big' : '')}>
    <${Select} label=${t('gp.preset')} value=${cur.id} options=${options} onChange=${(id: string) => pickPreset(s, id)} />
    ${changed && html`<button type="button" class="reset-arrow" title=${reset} aria-label=${reset}
      onClick=${() => pickPreset(s, tpl.id)}>↺</button>`}
  </div>`;
}

/** The drivers in seed order: the list, or the random draw. */
function seedOrder(x: TournamentTemplate): string[] {
  const names = named(gpDrivers.value).map(d => d.name);
  const random = x.format !== 'single' && x.split === 'random';
  const draw = gpDraw.value;
  const drawn = !!draw && draw.length === names.length && draw.every(n => names.includes(n));
  return random && drawn ? draw! : names;
}

function useDraw(x: TournamentTemplate) {
  const names = named(gpDrivers.value).map(d => d.name);
  const random = x.format !== 'single' && x.split === 'random';
  const draw = gpDraw.value;
  const drawn = !!draw && draw.length === names.length && draw.every(n => names.includes(n));
  useEffect(() => { if (random && !drawn) drawGroups(names); }, [random, drawn]);
  return { random, names };
}

const phaseSections = (ph: Phase) => ph.stages.length > 1 ? gpPhases.value[String(ph.column)]?.sections ?? DEFAULT_SECTIONS : 1;

function phaseOrder(pl: Planned, ph: Phase) {
  const unit = (s: Stage) => { const p = pl.presetOf(s); return p.rotation === 'block' ? p.racesPerLane : 1; };
  return runOrder(pl.plan, phaseSections(ph), s => pl.est.get(s.id)!.races, unit).filter(o => ph.stages.some(s => s.id === o.stage));
}

/** A stage's name in the Grand Prix: the final is the Grand Final. */
function gpStageName(pl: Planned, s: Stage): string {
  return pl.plan.groupIsFinal || s.kind === 'final' ? t('gp.grandFinal') : stageName(s);
}

/** Whether the overview can be opened: a template and a field it can plan. */
function overviewReady(): boolean {
  const x = chosenTemplate();
  const n = named(gpDrivers.value).length;
  return !!x && n >= 2 && !plannedFor(x, n).plan.problems.some(p => p.level === 'error');
}

function go(s: GpStep) { gpStep.value = s; window.scrollTo({ top: 0 }); }
function goRound(column: number, view: 'setup' | 'race', section = 0) {
  gpRound.value = column; gpView.value = view; gpSection.value = section; gpStep.value = 'round'; window.scrollTo({ top: 0 });
}

// --- timeline ------------------------------------------------------------------------
//
// One tab per step: the setup, then per phase its settings and one tab per
// section (a race page each: "Group A 1/2" …), then the podium. With many tabs
// the bar scrolls by itself: shortly after a step opens it slides so that at
// least the two tabs on either side of it are in view.

interface TlItem { key: string; label: string; sub?: string; open: boolean; go?: () => void }

const PHASE_KINDS_BY_FORMAT: Record<TournamentTemplate['format'], PhaseKind[]> = {
  single: ['finals'], groupsFinal: ['groups', 'finals'], groupsRepechageFinal: ['groups', 'second', 'finals'],
};

/** The scroll position that shows `i` with two tabs to each side (as far as there are), or null if they are in view. */
function scrollFor(list: HTMLElement, i: number): number | null {
  const items = [...list.children] as HTMLElement[];
  if (!items.length) return null;
  const first = items[Math.max(0, i - 2)], last = items[Math.min(items.length - 1, i + 2)];
  const left = list.scrollLeft, width = list.clientWidth;
  // The list is the tabs' offset parent (position: relative), so offsets are within it.
  const rightEdge = last.offsetLeft + last.offsetWidth;
  const leftEdge = first.offsetLeft;
  if (rightEdge > left + width) return rightEdge - width;
  if (leftEdge < left) return leftEdge;
  return null;
}

function Timeline({ at }: { at: string }) {
  const x = chosenTemplate();
  const n = named(gpDrivers.value).length;
  const ready = overviewReady();
  const pl = x && ready ? eveningPlan(x, n).pl : null;
  const phases = pl ? phasesOf(pl.plan) : null;
  const items: TlItem[] = [
    { key: 'template', label: t('gp.step.template'), sub: x ? templateName(x) : '', open: true, go: () => go('template') },
    { key: 'drivers', label: t('gp.step.drivers'), sub: n ? t('gp.nDrivers', { n }) : '', open: !!x, go: () => go('drivers') },
    { key: 'cars', label: t('gp.step.cars'), open: !!x, go: () => go('cars') },
    { key: 'groups', label: t('gp.step.groups'), open: ready, go: () => go('groups') },
  ];
  // Nothing after a race opens before that race has finished.
  let blocked = false;
  if (pl && phases) phases.forEach((ph, pi) => {
    // Only the first phase can be opened until racing is built.
    const open = gpStarted.value && pi === 0 && !blocked;
    items.push({ key: `setup:${ph.column}`, label: t(`gp.phase.${ph.kind}` as Key), sub: t('gp.step.settings'), open,
      go: () => goRound(ph.column, 'setup') });
    phaseOrder(pl, ph).forEach((o, si) => {
      const st = pl.plan.stages.find(q => q.id === o.stage)!;
      items.push({
        key: `race:${ph.column}:${si}`, open: open && !blocked, go: () => goRound(ph.column, 'race', si),
        label: gpStageName(pl, st) + (o.parts > 1 ? ` ${o.part + 1}/${o.parts}` : ''), sub: racesText(o.races),
      });
      if (!gpDone.value.includes(raceKey(o.stage, o.start))) blocked = true;
    });
  });
  else for (const k of x ? PHASE_KINDS_BY_FORMAT[x.format] : PHASE_KINDS_BY_FORMAT.groupsRepechageFinal)
    items.push({ key: `phase:${k}`, label: t(`gp.phase.${k}` as Key), open: false });
  items.push({ key: 'podium', label: t('gp.step.podium'), open: false });
  const now = items.findIndex(i => i.key === at);

  // Slide the bar shortly after a step opens (at once on the first show).
  const list = useRef<HTMLOListElement>(null);
  const shown = useRef(false);
  useEffect(() => {
    const el = list.current;
    if (!el || now < 0) return;
    const move = (smooth: boolean) => {
      const to = scrollFor(el, now);
      if (to !== null) el.scrollTo({ left: to, behavior: smooth ? 'smooth' : 'auto' });
    };
    if (!shown.current) { shown.current = true; move(false); return; }
    const id = setTimeout(() => move(true), 450);
    return () => clearTimeout(id);
  }, [at, items.length]);

  return html`<nav class="gp-timeline" aria-label=${t('gp.steps')}>
    <ol ref=${list}>${items.map((it, i) => {
      const state = i < now ? 'done' : i === now ? 'now' : 'todo';
      const can = it.key !== at && it.open && !!it.go;
      return html`<li key=${it.key} class=${'gp-tl ' + state + (it.key.startsWith('race:') ? ' race' : '')}>
        <button type="button" disabled=${!can && it.key !== at} aria-current=${it.key === at ? 'step' : undefined}
            onClick=${() => { if (can) it.go!(); }}>
          <span class="gp-tl-n tnum">${i + 1}</span>
          <span class="gp-tl-text"><span class="gp-tl-label">${it.label}</span>
            ${it.sub && html`<span class="gp-tl-sub">${it.sub}</span>`}</span>
        </button>
      </li>`;
    })}</ol>
  </nav>`;
}

function Foot({ back, next, nextLabel, nextDisabled, note, children, center }:
  { back?: GpStep | (() => void); next?: () => void; nextLabel?: string; nextDisabled?: boolean; note?: string; children?: unknown; center?: unknown }) {
  const onBack = typeof back === 'function' ? back : back ? () => go(back) : undefined;
  return html`<div class=${'gp-foot' + (center ? ' has-center' : '')}>
    ${center && html`<div class="gp-foot-center">${center}</div>`}
    <div class="gp-foot-left">${onBack && html`<${Button} onClick=${onBack}>← ${t('gp.back')}<//>`}${children}</div>
    <div class="gp-foot-right">
      ${note && html`<span class="gp-foot-note">${note}</span>`}
      ${next && html`<${Button} kind="primary" disabled=${nextDisabled} onClick=${next}>${nextLabel ?? t('gp.next')} →<//>`}
    </div>
  </div>`;
}

// --- step 1: tournament --------------------------------------------------------------

function Flow({ x }: { x: TournamentTemplate }) {
  const parts: Key[] = x.format === 'single' ? ['tt.kind.final']
    : x.format === 'groupsFinal' ? ['tt.kind.group', 'tt.kind.final'] : ['tt.kind.group', 'tt.kind.repechage', 'tt.kind.final'];
  return html`<span class="gp-flow">${parts.map((k, i) => html`${i > 0 && html`<span class="gp-flow-arrow" aria-hidden="true">→</span>`}<span class="gp-flow-step">${t(k)}</span>`)}</span>`;
}

function pickTemplate(id: string) {
  if (gpTemplateId.value !== id) resetEvening();
  gpTemplateId.value = id;
}

function TemplateCard({ x, on }: { x: TournamentTemplate; on: boolean }) {
  const byId = new Map(presets.value.map(p => [p.id, p]));
  const pn = (id: string) => { const p = byId.get(id); return p ? presetName(p) : '—'; };
  const facts: [Key, string][] = x.format === 'single' ? [['tt.kind.final', pn(x.finalPreset)]]
    : [['tt.kind.group', pn(x.groupPreset)],
       ...(x.format === 'groupsRepechageFinal' ? [['tt.kind.repechage', pn(x.repechagePreset)] as [Key, string]] : []),
       ['tt.kind.final', pn(x.finalPreset)]];
  return html`<button type="button" class=${'gp-tpl' + (on ? ' on' : '')} aria-pressed=${on}
      onClick=${() => pickTemplate(x.id)} onDblClick=${() => { pickTemplate(x.id); go('drivers'); }}>
    <span class="gp-slant">
      <span class="gp-tpl-head">
        <span class="gp-tpl-name">${templateName(x)}</span>
        ${x.builtIn ? html`<em class="md-item-tag">${t('rp.builtInTag')}</em>` : html`<em class="md-item-tag gp-own">${t('gp.ownTag')}</em>`}
      </span>
      <${Flow} x=${x} />
      <span class="gp-tpl-desc">${t(`gp.desc.${x.format}` as Key)}${x.format !== 'single' && ' ' + t(x.split === 'snake' ? 'gp.desc.snake' : 'gp.desc.random')}</span>
      <span class="gp-tpl-facts">${facts.map(([k, v]) => html`<span><small>${t(k)}</small>${v}</span>`)}</span>
    </span>
  </button>`;
}

function TemplateStep() {
  const list = templates.value;
  const x = chosenTemplate();
  return html`<section class="gp-step">
    <h2 class="gp-prompt">${t('gp.pick.title')}</h2>
    <div class="gp-tpls">${list.map(y => html`<${TemplateCard} key=${y.id} x=${y} on=${y.id === x?.id} />`)}</div>
    <p class="gp-aside"><a href="#/tournaments">${t('gp.pick.edit')}</a></p>
    <${Foot} next=${() => go('drivers')} nextDisabled=${!x} note=${x ? '' : t('gp.pick.none')} />
  </section>`;
}

// --- step 2: drivers -----------------------------------------------------------------

function NameCard({ d, i, list, sort }: { d: GpDriver; i: number; list: GpDriver[]; sort: ReturnType<typeof useSortable> }) {
  const [draft, setDraft] = useState(d.name);
  const ref = useRef<HTMLInputElement>(null);
  useLayoutEffect(() => setDraft(d.name), [d.name]);       // synchronously: a late reset would eat typed letters
  // Right after the card appears (not after the next paint), so fast typing lands in it.
  useLayoutEffect(() => {
    if (gpFocusId.value === d.id) { ref.current?.focus(); gpFocusId.value = null; }
  });
  const others = () => list.filter(o => o.id !== d.id).map(o => o.name).filter(Boolean);
  const commit = () => {
    const name = keepName(draft, others());
    if (!name && list.length > 1) { removeDriver(d.id); return; }
    setDraft(name);
    if (name !== d.name) renameDriver(d.id, name);
  };
  const keys = sort.controlProps(i, () => {}) as { onKeyDown: (e: KeyboardEvent) => void };
  return html`<li key=${d.id} ...${sort.itemProps(i)}>
    <div class=${'gp-name' + (d.name ? '' : ' empty')}>
      <span class="gp-seed tnum">${i + 1}.</span>
      <input ref=${ref} class="gp-name-input" value=${draft} maxlength=${NAME_MAX} placeholder=${t('gp.namePh')}
        aria-label=${t('gp.nameOf', { n: i + 1 })} spellcheck="false" autocomplete="off" enterkeyhint="next"
        aria-keyshortcuts="Alt+ArrowUp Alt+ArrowDown"
        onInput=${(e: Event) => {
          const el = e.target as HTMLInputElement;
          const v = typedName(el.value);
          if (v !== el.value) el.value = v;               // letters only: drop the rest as it is typed
          setDraft(v);
        }}
        onBlur=${commit}
        onKeyDown=${(e: KeyboardEvent) => {
          keys.onKeyDown(e);
          if (e.defaultPrevented) return;
          if (e.key === 'Enter') {
            e.preventDefault();
            const last = i === list.length - 1;
            (e.target as HTMLInputElement).blur();
            if (last && keepName(draft, others()) && named(gpDrivers.value).length < LIMITS.drivers.max) addDriver();
          } else if (e.key === 'Escape') setDraft(d.name);
        }} />
      <button type="button" class="gp-remove" title=${t('gp.remove')} aria-label=${t('gp.remove')}
        onClick=${() => list.length > 1 ? removeDriver(d.id) : (setDraft(''), renameDriver(d.id, ''))}>×</button>
      <span class="md-grip gp-grip" data-drag-handle title=${t('gp.dragHint')} aria-hidden="true"></span>
    </div>
  </li>`;
}

function SeedArrow() {
  return html`<div class="gp-arrow" aria-hidden="true">
    <span class="gp-arrow-label">${t('gp.fastest')}</span>
    <span class="gp-arrow-shaft"></span>
    <span class="gp-arrow-label">${t('gp.slowest')}</span>
  </div>`;
}

function DriversStep({ x }: { x: TournamentTemplate }) {
  const list = gpDrivers.value;
  useEffect(() => { if (!gpDrivers.value.length) addDriver(); }, [list.length === 0]);
  const sort = useSortable(list.length, moveDriver, { handleOnly: true });
  const n = named(list).length;
  const seeded = x.format !== 'single' && x.split === 'snake';
  const pl = n >= 2 ? plannedFor(x, n, { smallFinal: x.smallFinal }) : null;
  const errors = pl?.plan.problems.filter(p => p.level === 'error') ?? [];
  const full = n >= LIMITS.drivers.max;
  const groups = pl?.plan.stages.filter(s => s.kind === 'group') ?? [];
  const seconds = pl?.plan.stages.filter(s => s.kind === 'repechage') ?? [];
  const final = pl?.plan.stages.find(s => s.kind === 'final');

  const next = () => {
    gpDrivers.value = gpDrivers.value.filter(d => d.name);
    gpDraw.value = null;
    go('cars');
  };
  return html`<section class="gp-step">
    <div class="gp-drivers">
      <div class="gp-names-area">
        <h2 class="gp-prompt">${t('gp.drivers.title')}</h2>
        <p class="gp-lead">${t(x.format === 'single' ? 'gp.drivers.single' : seeded ? 'gp.drivers.seeded' : 'gp.drivers.random')}</p>
        <div class=${'gp-names-grid' + (seeded ? ' seeded' : '')}>
          ${seeded && html`<${SeedArrow} />`}
          <div>
            <ol ref=${sort.listRef} class=${'gp-names' + (sort.dragging ? ' sorting' : '')}>
              ${list.map((d, i) => html`<${NameCard} key=${d.id} d=${d} i=${i} list=${list} sort=${sort} />`)}
            </ol>
            <button type="button" class="md-add gp-add" disabled=${full} onClick=${() => addDriver()}>
              <span>+ ${t('gp.addDriver')}</span>
            </button>
            <p class="md-hint">${full ? t('gp.full', { n: LIMITS.drivers.max }) : t('gp.namesHint')}</p>
            <p class="visually-hidden" aria-live="polite">${sort.lastMove ? t('rp.moved', { n: sort.lastMove.to + 1, total: sort.lastMove.count }) : ''}</p>
          </div>
        </div>
      </div>
      <aside class="gp-side">
        <p class="gp-side-tpl">${templateName(x)}</p>
        <p class="gp-side-n"><span class="tnum">${n}</span> ${t('gp.driversWord')}</p>
        ${n < 2 ? html`<p class="gp-side-note">${t('gp.minDrivers')}</p>`
          : errors.length ? html`<div class="gp-side-err" role="alert">${errors.map(p => html`<p>${problemText(p)}</p>`)}</div>`
          : html`<ul class="gp-side-plan">
              ${groups.length > 0 && html`<li>${t('gp.side.groups', { g: groups.length, size: sizesText(groups.map(g => g.seats.length)) })}</li>`}
              ${seconds.length > 0 && html`<li>${t(seconds.length === 1 ? 'gp.side.second1' : 'gp.side.secondN', { m: seconds.length })}</li>`}
              ${final && html`<li>${t('gp.side.final', { n: final.seats.length })}</li>`}
            </ul>
            <dl class="gp-side-tot">
              <div><dt>${t('tt.tot.races')}</dt><dd class="tnum">${pl!.races}</dd></div>
              <div class="tt-total"><dt>${t('tt.tot.duration')}</dt><dd class="tnum">${formatDuration(pl!.ms)}</dd></div>
            </dl>`}
        <p class="gp-side-lanes">${t('tt.onLanes', { n: trackLanes.value })} <a href="#/track-setup">${t('tt.changeLanes')}</a></p>
      </aside>
    </div>
    <${Foot} back="template" next=${next} nextDisabled=${n < 2 || errors.length > 0} />
  </section>`;
}

// --- step 3: cars --------------------------------------------------------------------

/**
 * The cars for tonight: left the car presets (a click puts one's cars on the
 * lanes; the one that matches is marked), "Save as car preset", and a link to
 * the Car Presets area; right the board, where any car can still be changed
 * for tonight only.
 */
function CarsStep() {
  const lanes = trackLanes.value;
  const cars = gpCars.value;
  const list = carPresets.value;
  const match = list.find(p => sameCars(p.cars, cars, lanes));
  const [saved, setSaved] = useState<string | null>(null);
  return html`<section class="gp-step">
    <div class="gpc-head">
      <h2 class="gp-prompt">${t('gp.cars.title')}</h2>
      <p class="gp-lead">${t('gp.cars.lead')}</p>
    </div>
    <div class="md gpc-md">
      <nav class="md-list" aria-label=${t('area.carPresets')}>
        <h3 class="md-sub">${t('area.carPresets')}</h3>
        ${list.length ? html`<ul>${list.map(p => html`<li key=${p.id}>
          <button type="button" class=${'md-item fixed' + (p === match ? ' on' : '')} aria-pressed=${p === match}
              onClick=${() => { applyCarSet(p.cars); setSaved(null); }}>
            <span class="md-item-name">${p.name}</span>
            <${CarChips} cars=${p.cars} lanes=${lanes} />
          </button>
        </li>`)}</ul>` : html`<p class="md-hint">${t('gp.cars.noPresets')}</p>`}
        <button type="button" class="md-add" disabled=${!!match}
            onClick=${() => { const p = createCarPreset(cars, t('gp.cars.savedName')); setSaved(p.name); }}>
          <span>+ ${t('gp.cars.saveAs')}</span>
        </button>
        <p class="md-hint">${saved && match ? t('gp.cars.saved', { name: saved }) + ' ' : ''}<a href="#/car-presets">${t('gp.cars.edit')}</a></p>
      </nav>
      <div class="gpc-main">
        <p class="gpc-status">${match ? t('gp.cars.using', { name: match.name }) : t('gp.cars.own')}</p>
        <${CarBoard} lanes=${lanes} cars=${cars} onChange=${setCar} colors=${false} />
        <p class="gp-side-lanes gpc-lanes">${t('tt.onLanes', { n: lanes })} <a href="#/track-setup">${t('tt.changeLanes')}</a></p>
      </div>
    </div>
    <${Foot} back="drivers" next=${() => go('groups')} nextDisabled=${!overviewReady()} />
  </section>`;
}

// --- step 3: overview ---------------------------------------------------------------

function OverviewStep({ x }: { x: TournamentTemplate }) {
  const { random, names } = useDraw(x);
  const { pl, up, down, hasSmall } = eveningPlan(x, names.length);
  const first = phasesOf(pl.plan)[0];
  return html`<section class="gp-step">
    <div class="gp-overview-head">
      <h2 class="gp-prompt">${t('gp.overview.title')}</h2>
      <dl class="gp-overview-tot">
        <div><dt>${t('tt.tot.races')}</dt><dd class="tnum">${pl.races}</dd></div>
        <div class="tt-total"><dt>${t('tt.tot.duration')}</dt><dd class="tnum">${formatDuration(pl.ms)}</dd></div>
      </dl>
    </div>
    <div class="gp-overview"><${Tree} x=${x} pl=${pl} names=${seedOrder(x)} finalCtl=${hasSmall ? { up, down } : undefined}
      stageCtl=${(s: Stage) => html`<${PresetChoice} s=${s} />`} /></div>
    <${Foot} back="cars" nextLabel=${t('gp.groups.start')}
        next=${() => { gpStarted.value = true; goRound(first.column, 'setup'); }}>
      ${random && html`<${Button} onClick=${() => drawGroups(names)}>${t('gp.groups.redraw')}<//>`}
    <//>
  </section>`;
}

// --- a phase: its settings -------------------------------------------------------------

/**
 * One race setting: blue while it differs from the tournament template, with
 * a ↺ beside the control that goes back (`arrow: false` when the control
 * brings its own, like the preset dropdown).
 */
function Setting({ label, changed, def, onReset, hint, id, arrow = true, children }:
  { label: string; changed: boolean; def: string; onReset: () => void; hint?: string; id?: string; arrow?: boolean; children?: unknown }) {
  const reset = t('gp.round.resetTo', { v: def });
  return html`<div class=${'gp-setting' + (changed ? ' changed' : '')}>
    <${Field} label=${label} id=${id} hint=${hint}>
      <div class="gp-setting-ctl">${children}
        ${arrow && changed && html`<button type="button" class="reset-arrow" title=${reset} aria-label=${reset} onClick=${onReset}>↺</button>`}
      </div>
    <//>
  </div>`;
}

/** Shrinks its content (never grows it) to fit the box's max-height, so nothing scrolls. */
function Fit({ children }: { children?: unknown }) {
  const box = useRef<HTMLDivElement>(null);
  const [size, setSize] = useState<{ w: number; h: number; s: number } | null>(null);
  useLayoutEffect(() => {
    const el = box.current!, inner = el.firstElementChild as HTMLElement;
    const measure = () => {
      const w = inner.offsetWidth, h = inner.offsetHeight;
      const max = parseFloat(getComputedStyle(el).maxHeight);
      const s = h > 0 && max > 0 ? Math.min(1, max / h) : 1;
      setSize(p => p && p.w === w && p.h === h && p.s === s ? p : { w, h, s });
    };
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(inner);
    window.addEventListener('resize', measure);
    return () => { ro.disconnect(); window.removeEventListener('resize', measure); };
  }, []);
  const s = size?.s ?? 1;
  return html`<div ref=${box} class="gp-fit" style=${size ? `width:${size.w * s}px; height:${size.h * s}px` : ''}>
    <div class="gp-fit-in" style=${`transform: scale(${s})`}>${children}</div>
  </div>`;
}

/** The whole tournament tree in a pop-up, for information only (no dropdowns). */
function TreeDialog({ x, pl, onClose }: { x: TournamentTemplate; pl: Planned; onClose: () => void }) {
  const ref = useRef<HTMLDialogElement>(null);
  useEffect(() => { ref.current?.showModal(); }, []);
  return html`<dialog ref=${ref} class="gp-tree-dialog" aria-label=${templateName(x)} onClose=${onClose}
      onClick=${(e: MouseEvent) => { if (e.target === ref.current) ref.current!.close(); }}>
    <div class="gp-tree-dialog-head">
      <h2>${templateName(x)}</h2>
      <button type="button" class="gp-tree-close" title=${t('gp.tree.close')} aria-label=${t('gp.tree.close')}
        onClick=${() => ref.current!.close()}>×</button>
    </div>
    <div class="gp-overview"><${Tree} x=${x} pl=${pl} names=${seedOrder(x)} /></div>
  </dialog>`;
}

/**
 * A window onto the tree: just this phase's column, with tags naming where its
 * places go (and where its seats come from). Click opens the whole tree.
 */
function TreePortal({ x, pl, ph }: { x: TournamentTemplate; pl: Planned; ph: Phase }) {
  const [open, setOpen] = useState(false);
  return html`<div class="gp-portal-wrap">
    <button type="button" class="gp-portal gp-overview" title=${t('gp.portal.open')} aria-label=${t('gp.portal.open')}
        onClick=${() => setOpen(true)}>
      <${Fit}><${Tree} x=${x} pl=${pl} names=${seedOrder(x)} focus=${ph.column} /><//>
      <span class="gp-portal-hint" aria-hidden="true">${t('gp.portal.hint')}</span>
    </button>
    ${open && html`<${TreeDialog} x=${x} pl=${pl} onClose=${() => setOpen(false)} />`}
  </div>`;
}

function PhaseOrder({ pl, ph }: { pl: Planned; ph: Phase }) {
  return html`<ol class="gp-order">${phaseOrder(pl, ph).map((o, i) => {
    const s = pl.plan.stages.find(q => q.id === o.stage)!;
    const ms = racesEstimateMs(pl.presetOf(s), s.seats.length, o.start, o.races, avgLapMs.value, changeoverMs.value);
    return html`<li key=${o.stage + o.part} class=${'gp-order-item k-' + s.kind}>
      <span class="gp-order-n tnum">${i + 1}</span>
      <span class="gp-order-name">${gpStageName(pl, s)}${o.parts > 1 && html` <small class="tnum">${o.part + 1}/${o.parts}</small>`}</span>
      <span class="gp-order-sub tnum">${racesText(o.races)} · ${formatDuration(ms)}</span>
    </li>`;
  })}</ol>`;
}

function RoundSetup({ x, pl, ph }: { x: TournamentTemplate; pl: Planned; ph: Phase }) {
  const col = String(ph.column);
  const cur = gpPhases.value[col];
  const base = basePreset(ph.stages[0]);
  const tpl = templatePreset(ph.stages[0]);
  const eff = applyPhase(base, cur);
  const changed = changedFields(base, cur);
  const quali = isQualifying(base);
  const set = <K extends 'laps' | 'durationMs' | 'racesPerLane' | 'sections'>(k: K, v: number | undefined) => {
    gpPhases.value = { ...gpPhases.value, [col]: setPhaseField(cur, base, k, v as never) };
  };
  const multi = ph.stages.length > 1;
  const sections = phaseSections(ph);
  const races = ph.stages.reduce((a, s) => a + pl.est.get(s.id)!.races, 0);
  const ms = ph.stages.reduce((a, s) => a + pl.est.get(s.id)!.ms, 0);
  const lenText = (p: RacePreset) => p.mode === 'laps' ? t(p.laps === 1 ? 'rp.sum.lap1' : 'rp.sum.laps', { n: p.laps }) : t('rp.sum.minutes', { n: p.durationMs / 60_000 });
  return html`<section class="gp-step">
    <h2 class="gp-prompt">${t(`gp.phaseTitle.${ph.kind}` as Key)}</h2>
    <p class="gp-lead">${multi ? t('gp.round.same', { stages: ph.stages.map(s => gpStageName(pl, s)).join(' · ') }) : t('gp.round.one', { stage: gpStageName(pl, ph.stages[0]) })}
      ${' '}${t('gp.round.lead')}</p>
    <div class="gp-round">
      <${TreePortal} x=${x} pl=${pl} ph=${ph} />
      <div class="gp-round-settings">
        <${Setting} label=${t('gp.preset')} changed=${base.id !== tpl.id} def=${presetName(tpl)} arrow=${false}
            hint=${t(quali ? 'rp.kind.qualifying.hint' : 'rp.kind.race.hint')} onReset=${() => pickPreset(ph.stages[0], tpl.id)}>
          <${PresetChoice} s=${ph.stages[0]} big />
        <//>
        ${!quali
          ? html`<${Setting} id="gp-laps" label=${t('rp.laps')} changed=${changed.has('laps')} def=${lenText({ ...base, mode: 'laps' })}
                onReset=${() => set('laps', undefined)}>
              <${Stepper} id="gp-laps" label=${t('rp.laps')} value=${eff.laps} min=${PRESET_LIMITS.laps.min} max=${PRESET_LIMITS.laps.max}
                onChange=${(v: number) => set('laps', v)} />
            <//>`
          : html`<${Setting} id="gp-min" label=${t('rp.minutes')} changed=${changed.has('durationMs')} def=${lenText({ ...base, mode: 'time' })}
                onReset=${() => set('durationMs', undefined)}>
              <${Stepper} id="gp-min" label=${t('rp.minutes')} unit="min" value=${eff.durationMs / 60_000}
                min=${PRESET_LIMITS.minutes.min} max=${PRESET_LIMITS.minutes.max} onChange=${(v: number) => set('durationMs', v * 60_000)} />
            <//>`}
        <${Setting} id="gp-k" label=${t('rp.racesPerLane')} changed=${changed.has('racesPerLane')} def=${String(base.racesPerLane)}
            hint=${t('rp.racesPerLane.hint', { k: eff.racesPerLane })} onReset=${() => set('racesPerLane', undefined)}>
          <${Stepper} id="gp-k" label=${t('rp.racesPerLane')} value=${eff.racesPerLane}
            min=${PRESET_LIMITS.racesPerLane.min} max=${PRESET_LIMITS.racesPerLane.max} onChange=${(v: number) => set('racesPerLane', v)} />
        <//>
      </div>
      <aside class="gp-side">
        <p class="gp-side-tpl">${t(`gp.phase.${ph.kind}` as Key)}</p>
        <dl class="gp-side-tot">
          <div><dt>${t('gp.round.races')}</dt><dd class="tnum">${races}</dd></div>
          <div class="tt-total"><dt>${t('gp.round.duration')}</dt><dd class="tnum">${formatDuration(ms)}</dd></div>
        </dl>
        ${multi && html`<${Setting} label=${t('gp.sections')} changed=${sections !== DEFAULT_SECTIONS} def=${String(DEFAULT_SECTIONS)}
            hint=${t(sections === 1 ? 'gp.sections.one' : 'gp.sections.hint', { n: sections })} onReset=${() => set('sections', undefined)}>
          <${Segmented} label=${t('gp.sections')} value=${String(sections)} onChange=${(v: string) => set('sections', Number(v))}
            options=${SECTION_CHOICES.map(n => ({ value: String(n), label: n === 1 ? t('gp.sections.none') : String(n) }))} />
        <//>`}
        <h3 class="gp-then">${t('gp.round.order')}</h3>
        <${PhaseOrder} pl=${pl} ph=${ph} />
      </aside>
    </div>
    <${Foot} back="groups" next=${() => goRound(ph.column, 'race', 0)} nextLabel=${t('gp.round.toRace')} />
  </section>`;
}

// --- a phase: the race page ---------------------------------------------------------------

/**
 * A driver's name in its box: a name too wide for it (up to 12 letters, some
 * wide) gets a smaller font (--fit) instead of being cut. Measured again when
 * the window or the font changes.
 */
function FitName({ text }: { text: string }) {
  const ref = useRef<HTMLSpanElement>(null);
  useLayoutEffect(() => {
    const el = ref.current;
    if (!el) return;
    const fit = () => {
      el.style.removeProperty('--fit');
      const cs = getComputedStyle(el);
      const pad = parseFloat(cs.paddingLeft) + parseFloat(cs.paddingRight);
      const over = (el.scrollWidth - pad) / Math.max(1, el.clientWidth - pad);
      if (over > 1) el.style.setProperty('--fit', String(Math.max(0.4, 0.97 / over)));
    };
    fit();
    void document.fonts?.ready.then(fit);
    // The font follows the window height: measure again whenever the page's size changes.
    const ro = new ResizeObserver(fit);
    ro.observe(document.documentElement);
    return () => ro.disconnect();
  }, [text]);
  return html`<span ref=${ref}>${text}</span>`;
}

/** The cars' run back to the left edge when the warm-up starts, before they drive. */
const INTRO_MS = 600;

/** When the last car of this warm-up reaches the line on screen (Date.now() time). */
function driveEnd(w: { startedAt: number; seed: number; driveMs: number }, lanes: readonly number[]): number {
  return w.startedAt + INTRO_MS + Math.max(0, ...lanes.map(l => carDrive(w.seed, l, w.driveMs).durationMs));
}

/**
 * A car on its lane. Standing: nose just before the line. During the warm-up
 * it runs quickly back to the left edge, then drives towards the line at its
 * own pace (src/grandPrix/warmup.ts carDrive) and stops before it; a page
 * opened later joins the drive where it is now.
 */
function LaneCar({ label, car, drive, startedAt }: { label: string; car: CarSetup; drive: CarDrive | null; startedAt: number }) {
  const ref = useRef<HTMLSpanElement>(null);
  useLayoutEffect(() => {
    const el = ref.current;
    if (!el || !drive || typeof el.animate !== 'function') return;
    const total = INTRO_MS + drive.durationMs;
    const at = (ms: number) => ms / total;
    const frames: Keyframe[] = [
      { offset: 0, left: '100%', easing: 'ease-in-out' },
      { offset: at(INTRO_MS), left: '0%', easing: 'linear' },
      ...drive.points.slice(1).map((q, i, all) => ({
        offset: at(INTRO_MS + q.t * drive.durationMs), left: `${q.p * 100}%`,
        easing: i === all.length - 2 ? 'ease-out' : 'linear',
      })),
    ];
    const anim = el.animate(frames, { duration: total, fill: 'forwards' });
    anim.currentTime = Math.min(total, Math.max(0, Date.now() - startedAt));
    return () => anim.cancel();
  }, [drive?.durationMs, startedAt]);
  return html`<span class="rb-run"><span ref=${ref} class="rb-car" role="img" aria-label=${label}><${Car} type=${car.type} label=${car.label} /></span></span>`;
}

/**
 * Re-renders the calling component about 30 times a second while `on` (a
 * timer, not animation frames: those stop in a hidden window, and the clocks
 * and lights must still be right when it shows again).
 */
function useFrames(on: boolean): void {
  const [, tick] = useState(0);
  useEffect(() => {
    if (!on) return;
    const id = setInterval(() => tick(n => n + 1), 33);
    return () => clearInterval(id);
  }, [on]);
}

/** A time with every digit in a box of its own width, so nothing beside it moves while it runs. */
function Digits({ text }: { text: string }) {
  return html`<b>${[...text].map(ch => html`<i class=${/[0-9–]/.test(ch) ? 'd' : 's'}>${ch}</i>`)}</b>`;
}

/**
 * A lane's times where the track was (src/grandPrix/raceClock.ts), always at
 * full width (0:00.00): left the lap in progress in the lane colour, or a
 * just-finished lap held for 5 s, framed so its driver spots it on the screen;
 * right, a little apart, the lane's best lap in white, purple when it is the
 * fastest lap of the race.
 */
function LaneClock({ car, toutUs, offsetMs, fastest }: {
  car: CarResult | undefined; toutUs: number | null; offsetMs: number | null; fastest: RaceState['fastestLap'];
}) {
  const nowUs = offsetMs === null ? 0 : (Date.now() - offsetMs) * 1000;
  const lastEnd = car?.lapTimes.at(-1)?.endUs;
  // A lap that just ended (also the last one, at the flag) is framed while it is held.
  const fresh = offsetMs !== null && lastEnd !== undefined && nowUs - lastEnd < LAST_LAP_HOLD_MS * 1000;
  useFrames(toutUs !== null && (car?.status !== 'finished' || fresh));
  const c = offsetMs === null ? { kind: 'idle' as const } : lapClock(car, toutUs, nowUs);
  const held = c.kind === 'last' || c.kind === 'finished';
  const purple = (n: number) => !!fastest && !!car && fastest.lane === car.lane && fastest.n === n;
  const best = car?.bestLapMs ?? null;
  const none = '–:––.––';
  return html`<span class="rb-clock">
    <span class=${'rb-cur ' + c.kind + (held && purple(c.n) ? ' fastest' : '') + (held && fresh ? ' fresh' : '')}>
      <${Digits} text=${c.kind === 'idle' ? formatLap(0) : c.kind === 'live' ? formatLap(c.ms) : c.ms === null ? none : formatLap(c.ms)} />
      ${held && html`<small class=${c.kind === 'finished' ? 'flag' : ''} title=${c.kind === 'finished' ? t('gp.run.flag') : t('gp.run.lap', { n: c.n })}>
        ${c.kind === 'finished' ? '' : t('gp.run.lapShort', { n: c.n })}</small>`}
    </span>
    <span class=${'rb-best' + (best === null ? ' none' : purple(car!.bestLapN!) ? ' fastest' : '')}>
      <${Digits} text=${best === null ? none : formatLap(best)} />
    </span>
  </span>`;
}

/**
 * The start/finish line from above, turned so the lanes stack 1 (top) … L:
 * per lane the driver's name (a parallelogram in the lane colour), the car
 * (nose at the line), the line, the lane number and its light (red, like the
 * track's), the light on the middle of the line, all at the right edge;
 * the room at the left holds a huge "Take / seats / warm-up". The line and
 * the numbers never move: when the warm-up starts the text fades, and the
 * names, the track's edge and the cars slide to the left edge (the track
 * stretches up to the line). In the race the positions take the left. Until a stage's first warm-up the host can take a lane out
 * by clicking it (car, line and number): a red cross draws over it on hover,
 * and its car moves to the first free lane with the name.
 */
function LaneBoard({ lanes, seats, names, lamps, levels, onDisable, warm, race }: {
  lanes: number; seats: readonly { lane: number; driver: number }[]; names: readonly string[];
  /** The lanes whose light is on. */
  lamps: readonly number[];
  /** During the start sequence: each lane's light level 0..1 (index 0 = lane 1), instead of `lamps`. */
  levels: readonly number[] | null;
  /** Null when the lanes are fixed (warm-up started, not the stage's first race, or no free lane). */
  onDisable: ((lane: number) => void) | null;
  /** During the warm-up: its laps, each lane's state and the cars' drive on screen. */
  warm: { laps: number; lane: (l: number) => LaneWarmup; drive: (l: number) => CarDrive | null; startedAt: number } | null;
  /** During the race: laps to drive (null in a time race), each car's result, lights out and the clock offset. */
  race: {
    laps: number | null; car: (l: number) => CarResult | undefined; toutUs: number | null; offsetMs: number | null;
    fastest: RaceState['fastestLap']; standings: RaceState['standings']; flagOut: boolean; code: (l: number) => string;
  } | null;
}) {
  const byLane = new Map(seats.map(x => [x.lane, x.driver]));
  return html`<div class=${'rb' + (warm || race ? ' warm' : '') + (race ? ' race' : '')} style=${`--rows:${lanes}`}>
    ${!race && html`<p class=${'rb-call' + (warm ? ' gone' : '')} aria-hidden="true">
      <span>${t('gp.call.1')}</span><span>${t('gp.call.2')}</span><b>${t('gp.call.3')}</b></p>`}
    ${race && html`
      <${Tower} race=${race} />
      <div class="rb-colhead" aria-hidden="true"><span>${t('gp.run.current')}</span><span>${t('gp.run.best')}</span></div>`}
    ${Array.from({ length: lanes }, (_, i) => {
      const l = i + 1;
      const d = byLane.get(l);
      const lit = d !== undefined;
      return html`<div key=${l} class=${'rb-row' + (lit ? '' : ' empty') + (i === 0 ? ' first' : '') + (i === lanes - 1 ? ' last' : '')}
          style=${`--lane: var(--lane-${l}); --ink: var(--lane-${l}-ink, #111); --fg: var(--lane-${l}-fg, var(--lane-${l})); --i: ${i}`}>
        <div class="rb-pre"></div>
        <div class="rb-name">${lit ? html`<${FitName} text=${names[d!]} />` : html`<span>${t('gp.race.empty')}</span>`}</div>
        <div class="rb-cnt">${race && lit ? (() => {
          // The lap the car is on, F1 style: 0/5 on the grid, 1/5 from its start crossing.
          const c = race.car(l);
          const n = lapOn(c, race.laps);
          return html`<span key="race" class=${'rb-count tnum' + (c?.status === 'finished' ? ' done' : '')}
              aria-label=${race.laps === null ? t('gp.run.lapsDone', { n }) : t('gp.run.count', { n, m: race.laps })}>
            <b>${n}</b>${race.laps !== null && html`<i>/</i>${race.laps}`}</span>`;
        })() : warm && lit && (() => {
          const w = warm.lane(l);
          return w.showCount && html`<span class=${'rb-count tnum' + (w.over ? ' over' : '')}
              title=${w.over ? t('gp.warm.over', { name: names[d!] }) : ''} aria-label=${t('gp.warm.count', { n: w.lap, m: warm.laps })}>
            <b>${w.lap}</b><i>/</i>${warm.laps}</span>`;
        })()}</div>
        <div class="rb-lane">
          <span class="rb-road" aria-hidden="true"><span class="rb-slot"></span></span>
          ${race && lit && html`<${LaneClock} car=${race.car(l)} toutUs=${race.toutUs} offsetMs=${race.offsetMs} fastest=${race.fastest} />`}
          ${lit && html`<${LaneCar} label=${t('gp.race.carLabel', { name: names[d!], lane: l })} car=${carOf(l)}
            drive=${race ? null : warm?.drive(l) ?? null} startedAt=${warm?.startedAt ?? 0} />`}
        </div>
        <div class="rb-line">
          ${levels ? html`<span class="rb-lamp" style=${`--lvl: ${(levels[i] ?? 0).toFixed(3)}`}></span>`
            : html`<span class=${'rb-lamp' + (lamps.includes(l) ? ' on' : '')}
              title=${t(!lamps.includes(l) ? 'gp.race.lampOff' : warm ? 'gp.race.lampStop' : 'gp.race.lampOn')}></span>`}
        </div>
        <div class="rb-num tnum"><span>${l}</span></div>
        ${onDisable && lit && html`<button type="button" class="rb-x" title=${t('gp.race.disableLabel', { lane: l, name: names[d!] })}
            aria-label=${t('gp.race.disableLabel', { lane: l, name: names[d!] })} onClick=${() => onDisable(l)}>
          <svg aria-hidden="true">
            <line x1="0" y1="0" x2="100%" y2="100%" pathLength="1" /><line x1="100%" y1="0" x2="0" y2="100%" pathLength="1" />
          </svg>
        </button>`}
      </div>`;
    })}
  </div>`;
}

/**
 * A gap in the position table: "+1.23", "+1 lap", or none. Later, with a
 * Power Pack: a real time interval for a lapped car too, from where the cars
 * are on the track (how precise that is decides how it is shown).
 */
function gapText(g: { laps: number; ms: number | null } | null): string {
  if (!g) return '–';
  if (g.laps > 0) return t(g.laps === 1 ? 'gp.tower.lap1' : 'gp.tower.laps', { n: g.laps });
  if (g.ms === null) return '–';
  const s = g.ms / 1000;
  return '+' + (s < 60 ? s.toFixed(2) : `${Math.floor(s / 60)}:${(s % 60).toFixed(2).padStart(5, '0')}`);
}

/**
 * The position table at the left of the race page, F1 style: the lap the
 * leader is on, then per position the car's code in its lane colour and its
 * gap to the leader. Live: the rows stay in lane order in the page and each
 * sits at its position (--pos), so a change of places slides instead of jumping.
 */
function Tower({ race }: { race: NonNullable<Parameters<typeof LaneBoard>[0]['race']> }) {
  // The leader's lap, like the lane counters: 0 until the first car crosses the line.
  const lapNow = Math.max(0, ...race.standings.map(st => lapOn(race.car(st.lane), race.laps)));
  const head = race.flagOut ? t('gp.tower.flag') : race.laps === null ? t('gp.tower.lapOnly', { n: lapNow }) : t('gp.tower.lap', { n: lapNow, m: race.laps });
  // The leader's lap, big: the strip fills the room above the rows, so the table ends level with the last lane.
  return html`<div class="rb-tower" role="table" aria-label=${t('gp.tower.title')} style=${`--n: ${race.standings.length}`}>
    <div class=${'tw-head' + (race.flagOut ? ' flag' : '')} role="row" aria-label=${head}>
      ${race.flagOut ? html`<b class="tw-lap">${head}</b>`
        : html`<small>${t('gp.tower.lapWord')}</small><b class="tw-lap tnum"
            style=${`--chars: ${String(lapNow).length + (race.laps === null ? 0 : .55 * (1 + String(race.laps).length))}`}>${lapNow}${race.laps !== null && html`<i>/${race.laps}</i>`}</b>`}
    </div>
    <ol class="tw-rows">
      ${[...race.standings].sort((a, b) => a.lane - b.lane).map(st => ({ st, i: race.standings.indexOf(st) })).map(({ st, i }) => html`<li key=${st.lane} role="row" class=${'tw-row' + (i === 0 ? ' lead' : '') + (st.status === 'finished' ? ' done' : '')}
          style=${`--lane: var(--lane-${st.lane}); --fg: var(--lane-${st.lane}-fg, var(--lane-${st.lane})); --pos: ${i}`}>
        <span class="tw-pos tnum">${st.position}</span>
        <span class="tw-code">${race.code(st.lane)}</span>
        <span class="tw-gap tnum">${i === 0 ? (st.status === 'grid' ? '–' : t('gp.tower.leader')) : st.status === 'grid' ? '–' : gapText(st.gap)}</span>
      </li>`)}
    </ol>
  </div>`;
}

/**
 * Dev tools (Settings): a button per lane that acts like that car crossing
 * the line, during the warm-up and the race, and the keys 1–6 for the same.
 * Only without a time taker; with one, only real crossings count.
 */
function DevCrossings({ lanes, linked, onReset }: { lanes: readonly number[]; linked: boolean; onReset: () => void }) {
  useEffect(() => {
    if (linked) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.repeat || e.ctrlKey || e.metaKey || e.altKey || (e.target as HTMLElement)?.closest?.('input, textarea')) return;
      const l = Number(e.key);
      if (lanes.includes(l)) simulateCrossing(l);
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [lanes.join(','), linked]);
  return html`<div class="gp-dev" role="group" aria-label=${t('gp.dev.cross')}>
    <span class="gp-dev-tag">${t('gp.dev.title')}</span>
    ${linked ? html`<span class="gp-dev-note">${t('gp.dev.linked')}</span>` : html`
      <span class="gp-dev-note">${t('gp.dev.cross')}</span>
      ${lanes.map(l => html`<button type="button" class="gp-dev-btn tnum" style=${`--lane: var(--lane-${l})`}
        title=${t('gp.dev.crossLabel', { lane: l })} aria-label=${t('gp.dev.crossLabel', { lane: l })}
        onClick=${() => simulateCrossing(l)}>${l}</button>`)}`}
    <button type="button" class="gp-dev-btn gp-dev-reset" title=${t('gp.dev.resetLabel')} aria-label=${t('gp.dev.resetLabel')}
      onClick=${onReset}>${t('gp.dev.reset')}</button>
  </div>`;
}

/** Who is in a stage, by its seats: names for seeds, "3rd of Group A" for places. */
function stageDrivers(pl: Planned, s: Stage, order: readonly string[]): string[] {
  return s.seats.map(z => z.kind === 'seed' ? order[z.seed - 1] ?? '?'
    : t('tt.from.places', { places: ordinal(z.rank), stage: gpStageName(pl, pl.plan.stages.find(q => q.id === z.stage)!) }));
}

function RaceView({ x, pl, ph, section }: { x: TournamentTemplate; pl: Planned; ph: Phase; section: number }) {
  const lanes = trackLanes.value;
  const order = phaseOrder(pl, ph);
  const si = Math.min(Math.max(0, section), order.length - 1);
  const sec = order[si];
  const s = pl.plan.stages.find(q => q.id === sec.stage)!;
  const names = stageDrivers(pl, s, seedOrder(x));
  const stored = gpSeating.value[s.id];
  const seating = stored && stored.length === names.length && validSeating(stored, lanes) ? stored : defaultSeating(names.length, lanes);
  const p = pl.presetOf(s);
  const races = raceScheduleSeated(seating, p.racesPerLane, p.rotation);
  const raceIdx = sec.start;
  const race = races[raceIdx];
  // The free lane is chosen until the stage's first warm-up; after that the lanes are fixed.
  const free = inactiveLanes(seating, lanes);
  const locked = gpLocked.value.includes(s.id);
  const canChange = raceIdx === 0 && free.length > 0 && !locked;
  const used = race.seats.map(z => z.lane);
  const wu = warmupOf(s.id, raceIdx);
  // A warm-up only when the drivers sit down cold (a section's first race) or have just changed lanes.
  const warmLaps = warmupBefore(p, names.length, raceIdx, sec.start) ? p.warmupLaps : 0;
  const run = raceOf(s.id, raceIdx);
  const st = useMemo(() => run ? fold(run.config, run.events) : null, [run]);
  const starting = !!run && (!st || st.phase === 'waiting' || st.phase === 'starting');
  useFrames(starting);
  // Before the warm-up the used lanes are lit; during it a light goes (red) on as its car starts its
  // last warm-up lap; in the race the start sequence drives them (shown here as it runs), then a
  // light goes red as its car starts its last lap (lapped races; time races later).
  const lamps = run ? (st && !starting && p.mode === 'laps' ? finalLapLanes(st.cars, p.laps) : [])
    : wu ? stopLanes(wu.state, used, warmLaps) : used;
  const levels = starting ? startLightLevels(run!.config.startLights,
    planStartLights(run!.config.startLights, run!.lights, Infinity), Date.now() - run!.t0Local) : null;
  const key = lamps.join(',');
  useEffect(() => { wantedLaneLights.value = lamps; }, [key]);
  useEffect(() => () => { wantedLaneLights.value = null; }, []);
  const startWarm = () => beginWarmup(s.id, raceIdx, used, warmupDriveMs(avgLapMs.value, warmLaps));
  // Once every car has reached the line on screen, "Warm-up running" becomes "Start race".
  const end = wu ? driveEnd(wu, used) : 0;
  const [, rerender] = useState(0);
  useEffect(() => {
    if (!wu || Date.now() >= end) return;
    const id = window.setTimeout(() => rerender(n => n + 1), end - Date.now() + 20);
    return () => clearTimeout(id);
  }, [end]);
  const driven = !!wu && Date.now() >= end;
  const startRace = () => {
    const entries = race.seats.map(z => ({ lane: z.lane, driver: names[z.driver] }));
    const id = `${s.id}-${raceIdx + 1}-${Date.now().toString(36)}`;
    beginRace(s.id, raceIdx, resolveConfig(toRaceConfig(p, x.rules, trackTiming.value, id, entries,
      `${gpStageName(pl, s)} · ${t('gp.race.title', { n: raceIdx + 1, total: races.length })}`)), lanes);
  };
  const phase = st?.phase;
  const ab = raceAborted.value;
  const aborted = !run && !!ab && ab.stage === s.id && ab.race === raceIdx;
  const codes = useMemo(() => {
    const list = shortCodes(race.seats.map(z => names[z.driver]));
    return new Map(race.seats.map((z, i) => [z.lane, list[i]]));
  }, [race.seats.map(z => names[z.driver] + z.lane).join('|')]);
  const link = trackLink.value;
  const len = p.mode === 'laps' ? t(p.laps === 1 ? 'rp.sum.lap1' : 'rp.sum.laps', { n: p.laps }) : t('rp.sum.minutes', { n: p.durationMs / 60_000 });
  const disable = (lane: number) => {
    const d = race.seats.find(z => z.lane === lane)?.driver;
    if (d === undefined || !free.length) return;
    gpSeating.value = { ...gpSeating.value, [s.id]: moveToLane(seating, d, free[0], lanes) };
  };
  return html`<section class="gp-step gp-race">
    <div class="gp-race-head">
      <div>
        <p class="gp-race-kicker">${gpStageName(pl, s)}${sec.parts > 1 && html` · ${t('gp.race.section', { n: sec.part + 1, m: sec.parts })}`}</p>
        <h2 class="gp-prompt">${t('gp.race.title', { n: raceIdx + 1, total: races.length })}</h2>
      </div>
      <p class=${'gp-race-len' + (run ? ' gone' : '')}>${len}${warmLaps > 0 && html` · ${warmLaps === 1 ? t('gp.race.warmup1') : t('gp.race.warmup', { n: warmLaps })}`}</p>
    </div>
    <${LaneBoard} lanes=${lanes} seats=${race.seats} names=${names} lamps=${lamps} levels=${levels}
      onDisable=${canChange && !wu ? disable : null}
      race=${run && st ? {
        laps: p.mode === 'laps' ? p.laps : null, toutUs: st.start?.toutUs ?? null, offsetMs: run.offsetMs,
        car: (l: number) => st.cars.find(c => c.lane === l), fastest: st.fastestLap,
        standings: st.standings, flagOut: st.flagOut, code: (l: number) => codes.get(l) ?? String(l),
      } : null}
      warm=${wu ? {
        laps: warmLaps, startedAt: wu.startedAt,
        lane: (l: number) => laneWarmup(wu.state.counts[l] ?? 0, warmLaps),
        drive: (l: number) => carDrive(wu.seed, l, wu.driveMs),
      } : null} />
    <div class="gp-race-info">
    <p class="gp-aside gp-race-note">${run ? html`${t(starting ? 'gp.run.lights' : phase === 'finished' ? 'gp.run.done' : 'gp.run.hint')}${run.local ? ` ${t(devTools.value ? 'gp.run.localDev' : 'gp.run.local')}` : ''}`
        : raceStartFailed.value ? t('gp.run.failed')
        : aborted ? t('gp.run.stopped')
        : wu ? t(warmLaps === 1 ? 'gp.warm.hint1' : warmLaps === 0 ? 'gp.warm.hint0' : 'gp.warm.hint')
        : html`${canChange ? t('gp.race.disableHint', { stage: gpStageName(pl, s) })
          : free.length ? t('gp.race.locked', { stage: gpStageName(pl, s) }) : t('gp.race.allLanes')}
          ${' '}${t('gp.warm.ready')}`}
      ${run ? '' : html`${' '}${!link ? t('gp.race.lights.none') : !laneLightsOk.value ? t('gp.race.lights.failed') : wu ? '' : t('gp.race.lights.on')}
      ${wu && warmLaps > 0 ? ` ${t('gp.warm.estimate')}` : ''}`}</p>
    ${devTools.value && (wu || run) && html`<${DevCrossings} lanes=${used} linked=${!!link} onReset=${() => resetToPreRace(s.id, raceIdx)} />`}
    </div>
    <${Foot} back=${() => si > 0 ? goRound(ph.column, 'race', si - 1) : goRound(ph.column, 'setup')}
      center=${run
        ? starting
          // Pressed again during the start lights: they stop, and the button offers to restart them.
          ? html`<button type="button" class="gp-warm-btn running stoppable" title=${t('gp.run.stopLights')} onClick=${abortStart}>
              <span>${t('gp.run.starting')}</span></button>`
          : html`<button type="button" class=${'gp-warm-btn running' + (phase === 'finished' ? ' done' : '')} disabled>
              <span>${t(phase === 'finished' ? 'gp.run.finished' : 'gp.run.running')}</span></button>`
        : driven
        ? html`<button type="button" class="gp-warm-btn" onClick=${startRace}><span>${t(aborted ? 'gp.race.restart' : 'gp.race.start')}</span></button>`
        : html`<button type="button" class=${'gp-warm-btn' + (wu ? ' running' : '')} disabled=${!!wu} onClick=${startWarm}>
          <span>${t(wu ? 'gp.warm.running' : 'gp.warm.start')}</span></button>`}
      next=${() => {}} nextDisabled />
  </section>`;
}

// --- page ----------------------------------------------------------------------------

export function GrandPrixPage() {
  if (!templatesLoaded.value) return html`<div class="gp"></div>`;
  const x = chosenTemplate();
  let step = gpStep.value;
  if (step !== 'template' && !x) step = 'template';
  if ((step === 'groups' || step === 'round') && !overviewReady()) step = 'drivers';
  if (step === 'round' && !gpStarted.value) step = 'groups';
  let body: unknown, at: string = step;
  if (step === 'template') body = html`<${TemplateStep} />`;
  else if (step === 'drivers') body = html`<${DriversStep} x=${x!} />`;
  else if (step === 'cars') body = html`<${CarsStep} />`;
  else if (step === 'groups') body = html`<${OverviewStep} x=${x!} />`;
  else {
    const { pl } = eveningPlan(x!, named(gpDrivers.value).length);
    const phases = phasesOf(pl.plan);
    const ph = phases.find(p => p.column === gpRound.value) ?? phases[0];
    const race = gpView.value === 'race';
    const secs = phaseOrder(pl, ph);
    // Not past the first section whose race has not finished (the timeline is closed beyond it).
    const firstOpen = secs.findIndex(o => !gpDone.value.includes(raceKey(o.stage, o.start)));
    const si = Math.min(gpSection.value, Math.max(0, secs.length - 1), firstOpen < 0 ? Infinity : firstOpen);
    at = race ? `race:${ph.column}:${si}` : `setup:${ph.column}`;
    body = race ? html`<${RaceView} x=${x!} pl=${pl} ph=${ph} section=${si} />` : html`<${RoundSetup} x=${x!} pl=${pl} ph=${ph} />`;
  }
  // Each car's colour is its lane's colour in this Grand Prix.
  return html`<div class="gp" style=${laneColorVars()}>
    <header class="gp-head">
      <h1>${t('area.grandPrix')}</h1>
      <${Timeline} at=${at} />
    </header>
    ${body}
  </div>`;
}
