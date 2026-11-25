// Race Presets (GUI_SPEC §4): master–detail. Left the list in the host's
// order (drag and drop, Alt+arrows; "+ New preset" at the end); right the
// editor of the selected preset with a live summary card. Every change
// saves at once.
import { useEffect, useState } from 'preact/hooks';
import { html } from '../lib/html.ts';
import { t, type Key } from '../i18n/i18n.ts';
import { avgLapMs, changeoverMs, lang } from '../lib/prefs.ts';
import {
  presets, presetsOfKind, presetsLoaded, presetsPersistent, selectedPresetId, presetName,
  createPreset, duplicate, updatePreset, deletePreset, resetPreset, movePreset, usedBy, saveAsNew,
} from '../lib/presets.ts';
import {
  LIMITS, MAX_PLACES, PRESET_KINDS, POINTS_TABLES, pointsTableOf, isAtDefaults, builtInDefault, isQualifying, isTiebreaker, racesFor, raceEstimateMs, groupEstimateMs,
  type PointsTableId, type PresetKind, type RacePreset,
} from '../../src/presets/preset.ts';
import { AREAS } from '../shell/areas.ts';
import { trackLanes } from '../lib/trackLanes.ts';
import { useSortable } from '../ui/sortable.ts';
import { PageHeader, Panel, Button, Segmented, Field, Stepper, ConfirmDialog, formatDuration } from '../ui/ui.ts';

/** A preset as a dropdown option, under its kind's heading. */
export function presetOption(p: RacePreset): { value: string; label: string; group: string } {
  return { value: p.id, label: presetName(p), group: t(`rp.kind.${p.kind}` as Key) };
}

export function presetSummary(p: RacePreset): string {
  const length = p.mode === 'laps'
    ? t(p.laps === 1 ? 'rp.sum.lap1' : 'rp.sum.laps', { n: p.laps })
    : t('rp.sum.minutes', { n: p.durationMs / 60_000 });
  return `${length} · ${t('rp.sum.perLane', { k: p.racesPerLane })}`;
}

// --- List --------------------------------------------------------------------

/** One kind's list: sortable, with "+ New …" for qualifyings and races (there is one tiebreaker). */
function KindList({ kind, selected }: { kind: PresetKind; selected: string }) {
  const list = presetsOfKind(kind);
  const sort = useSortable(list.length, (from, to) => movePreset(kind, from, to));
  const sortable = list.length > 1;
  return html`<section class=${'md-kind md-kind-' + kind} aria-label=${t(`rp.kind.${kind}` as Key)}>
    <h3 class="md-sub">${t(`rp.kind.${kind}` as Key)}</h3>
    <ul ref=${sort.listRef} class=${sort.dragging ? 'sorting' : ''}>
      ${list.map((p, i) => html`<li key=${p.id} ...${sortable ? sort.itemProps(i) : {}}>
        <button type="button" class=${'md-item' + (p.id === selected ? ' on' : '') + (sortable ? '' : ' fixed')} aria-current=${p.id === selected || undefined}
            aria-keyshortcuts=${sortable ? 'Alt+ArrowUp Alt+ArrowDown' : undefined}
            ...${sortable ? sort.controlProps(i, () => { selectedPresetId.value = p.id; }) : { onClick: () => { selectedPresetId.value = p.id; } }}>
          ${sortable && html`<span class="md-grip" data-drag-handle title=${t('rp.dragHint')} aria-hidden="true"></span>`}
          <span class="md-item-name">${presetName(p)}${p.builtIn && html`<em class="md-item-tag" title=${t('rp.builtIn')}>${t('rp.builtInTag')}</em>`}</span>
          <span class="md-item-sub">${presetSummary(p)}</span>
        </button>
      </li>`)}
    </ul>
    ${kind !== 'tiebreaker' && html`<button type="button" class="md-add" onClick=${() => createPreset(kind)}>
      <span>+ ${t(`rp.new.${kind}` as Key)}</span>
    </button>`}
    <p class="visually-hidden" aria-live="polite">${sort.lastMove ? t('rp.moved', { n: sort.lastMove.to + 1, total: sort.lastMove.count }) : ''}</p>
  </section>`;
}

/** The sidebar: qualifyings, races, the tiebreaker; three kinds that never mix. */
function PresetList({ selected }: { selected: string }) {
  return html`<nav class="md-list" aria-label=${t('rp.listLabel')}>
    ${PRESET_KINDS.map(kind => html`<${KindList} key=${kind} kind=${kind} selected=${selected} />`)}
    <p class="md-hint">${t('rp.dragHint')}</p>
  </nav>`;
}

// --- Name (inline rename) --------------------------------------------------------

/** ↺ beside a changed setting of a shipped preset: back to the shipped value. */
function ResetArrow({ onReset, def }: { onReset: () => void; def: string }) {
  const label = t('rp.resetField', { v: def });
  return html`<button type="button" class="reset-arrow" title=${label} aria-label=${label} onClick=${onReset}>↺</button>`;
}

/**
 * A setting of the editor. For a shipped preset whose value differs from the
 * shipped one (`def` given and `changed`): blue, with ↺ right of the control.
 */
function PField({ label, id, hint, changed = false, def, onReset, children }:
  { label: string; id?: string; hint?: unknown; changed?: boolean; def?: string; onReset?: () => void; children?: unknown }) {
  const on = changed && def !== undefined && !!onReset;
  return html`<div class=${'pe-def' + (on ? ' changed' : '')}>
    <${Field} label=${label} id=${id} hint=${hint}>
      <div class="pe-ctl">${children}${on && html`<${ResetArrow} def=${def!} onReset=${onReset!} />`}</div>
    <//>
  </div>`;
}

function NameField({ p, list }: { p: RacePreset; list: RacePreset[] }) {
  const shown = presetName(p);
  const [draft, setDraft] = useState(shown);
  useEffect(() => setDraft(shown), [p.id, shown]);
  const name = draft.trim();
  const taken = list.some(o => o.id !== p.id && [o.name, presetName(o)].some(n => n.trim().toLocaleLowerCase() === name.toLocaleLowerCase()));
  const problem: Key | null = !name ? 'rp.err.nameEmpty' : taken ? 'rp.err.nameTaken' : null;
  const commit = () => {
    if (problem) setDraft(shown);            // leaving an invalid name restores the last good one
    else if (name !== shown) updatePreset(p.id, { name });
  };
  const d = builtInDefault(p.id);
  const renamed = !!d && p.name !== d.name;
  return html`<div class=${'pe-name' + (renamed ? ' changed' : '')}>
    <div class="pe-ctl">
    <input class=${'pe-name-input' + (problem ? ' invalid' : '')} value=${draft} maxlength="48"
      aria-label=${t('rp.name')} aria-invalid=${!!problem} spellcheck="false"
      onInput=${(e: Event) => setDraft((e.target as HTMLInputElement).value)}
      onBlur=${commit}
      onKeyDown=${(e: KeyboardEvent) => {
        if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
        else if (e.key === 'Escape') { setDraft(shown); }
      }} />
    ${renamed && html`<${ResetArrow} def=${t(`preset.builtin.${p.id}` as Key)} onReset=${() => updatePreset(p.id, { name: d!.name })} />`}
    </div>
    ${problem && html`<p class="field-error" role="alert">${t(problem)}</p>`}
  </div>`;
}

// --- Editor ------------------------------------------------------------------------

function Editor({ p, list }: { p: RacePreset; list: RacePreset[] }) {
  const set = (patch: Partial<RacePreset>) => updatePreset(p.id, patch);
  const [confirmDelete, setConfirmDelete] = useState(false);
  const [confirmReset, setConfirmReset] = useState(false);
  const [confirmSave, setConfirmSave] = useState(false);
  const atDefaults = p.builtIn && isAtDefaults(p);
  // A shipped preset: what differs from its shipped values is blue, with ↺ beside it.
  const d = builtInDefault(p.id);
  const ch = (k: 'laps' | 'durationMs' | 'warmupLaps' | 'racesPerLane' | 'rotation') => !!d && p[k] !== d[k];
  const back = (k: 'laps' | 'durationMs' | 'warmupLaps' | 'racesPerLane' | 'rotation') => () => d && set({ [k]: d[k] });
  const lapsText = (n: number) => t(n === 1 ? 'rp.sum.lap1' : 'rp.sum.laps', { n });
  const rotText = (r: RacePreset['rotation']) => t(r === 'block' ? 'rp.rotation.block' : 'rp.rotation.everyRace');
  const pointsChanged = !!d && p.points.some((v, i) => v !== d.points[i]);
  const dTable = d ? pointsTableOf(d.points) : null;
  const users = usedBy(p.id);
  const table = pointsTableOf(p.points);
  const k = p.racesPerLane;
  const tb = isTiebreaker(p.id);
  const quali = isQualifying(p);

  return html`<div class="md-detail">
    <div class="pe-head">
      <${NameField} p=${p} list=${list} />
      <div class="pe-actions">
        ${!tb && html`<${Button} onClick=${() => duplicate(p)}>${t('rp.duplicate')}<//>`}
        ${p.builtIn && !tb && !atDefaults && html`<${Button} onClick=${() => setConfirmSave(true)}>${t('rp.saveAsNew')}<//>`}
        ${p.builtIn
          ? html`<${Button} disabled=${atDefaults} cls=${atDefaults ? '' : 'btn-changed'} onClick=${() => setConfirmReset(true)}>${t('rp.reset')}<//>`
          : html`<${Button} disabled=${users.length > 0} onClick=${() => setConfirmDelete(true)}>${t('rp.delete')}<//>`}
      </div>
    </div>
    <p class="pe-note">${t(tb ? 'rp.tiebreakerNote' : quali ? 'rp.kind.qualifying.hint' : 'rp.kind.race.hint')}</p>
    ${p.builtIn && html`<p class="pe-note">${t(atDefaults ? 'rp.builtInNote' : 'rp.builtInChanged')}</p>`}
    ${users.length > 0 && html`<p class="pe-note">${t('rp.inUse', { names: users.join(', ') })}</p>`}

    <div class="pe-grid">
      <div class="pe-sections">
        <${Panel} title=${t('rp.sec.length')}>
          <div class="field-row">
            ${!quali
              ? html`<${PField} label=${t('rp.laps')} id="pe-laps" changed=${ch('laps')} def=${d && lapsText(d.laps)} onReset=${back('laps')}>
                  <${Stepper} id="pe-laps" label=${t('rp.laps')} value=${p.laps} min=${LIMITS.laps.min} max=${LIMITS.laps.max}
                    onChange=${(laps: number) => set({ laps })} />
                <//>`
              : html`<${PField} label=${t('rp.minutes')} id="pe-min" changed=${ch('durationMs')}
                  def=${d && t('rp.sum.minutes', { n: d.durationMs / 60_000 })} onReset=${back('durationMs')}>
                  <${Stepper} id="pe-min" label=${t('rp.minutes')} value=${p.durationMs / 60_000} unit="min"
                    min=${LIMITS.minutes.min} max=${LIMITS.minutes.max}
                    onChange=${(m: number) => set({ durationMs: m * 60_000 })} />
                <//>`}
            <${PField} label=${t('rp.warmup')} id="pe-warmup" hint=${t('rp.warmup.hint')}
                changed=${ch('warmupLaps')} def=${d && String(d.warmupLaps)} onReset=${back('warmupLaps')}>
              <${Stepper} id="pe-warmup" label=${t('rp.warmup')} value=${p.warmupLaps}
                min=${LIMITS.warmupLaps.min} max=${LIMITS.warmupLaps.max}
                onChange=${(warmupLaps: number) => set({ warmupLaps })} />
            <//>
          </div>
        <//>

        <${Panel} title=${t('rp.sec.rotation')}>
          <div class="field-row">
            <${PField} label=${t('rp.racesPerLane')} id="pe-k" hint=${t('rp.racesPerLane.hint', { k })}
                changed=${ch('racesPerLane')} def=${d && String(d.racesPerLane)} onReset=${back('racesPerLane')}>
              <${Stepper} id="pe-k" label=${t('rp.racesPerLane')} value=${k} min=${LIMITS.racesPerLane.min} max=${LIMITS.racesPerLane.max}
                onChange=${(racesPerLane: number) => set({ racesPerLane })} />
            <//>
            ${k > 1 && html`<${PField} label=${t('rp.rotation')}
                hint=${t(p.rotation === 'block' ? 'rp.rotation.block.hint' : 'rp.rotation.everyRace.hint', { k })}
                changed=${ch('rotation')} def=${d && rotText(d.rotation)} onReset=${back('rotation')}>
              <${Segmented} label=${t('rp.rotation')} value=${p.rotation} onChange=${(rotation: RacePreset['rotation']) => set({ rotation })}
                options=${[{ value: 'block', label: t('rp.rotation.block') }, { value: 'everyRace', label: t('rp.rotation.everyRace') }]} />
            <//>`}
          </div>
        <//>

        ${!tb && html`<${Panel} title=${t('rp.sec.points')}>
          <${PField} label=${t('rp.pointsTable')} hint=${t('rp.points.hint')} changed=${pointsChanged}
              def=${d && (dTable ? t(`rp.points.${dTable}` as Key) : d.points.join('-'))} onReset=${() => d && set({ points: [...d.points] })}>
            <${Segmented} label=${t('rp.pointsTable')} value=${table ?? 'custom'}
              onChange=${(id: PointsTableId | 'custom') => { if (id !== 'custom') set({ points: [...POINTS_TABLES[id]] }); }}
              options=${[
                ...(['linear', 'wide', 'f1'] as const).map(id => ({ value: id, label: t(`rp.points.${id}` as Key) })),
                ...(table ? [] : [{ value: 'custom', label: t('rp.points.custom') }]),
              ]} />
          <//>
          <div class="points-list">
            ${Array.from({ length: MAX_PLACES }, (_, i) => {
              const setPt = (v: number) => set({ points: p.points.map((x, j) => j === i ? v : x) });
              const rowChanged = !!d && p.points[i] !== d.points[i];
              return html`<div class=${'points-row pe-def' + (rowChanged ? ' changed' : '')} key=${i}>
                <label class="points-place" for=${'pe-pt' + i}>${t(`rp.place.${i + 1}` as Key)}</label>
                <${Stepper} id=${'pe-pt' + i} label=${t('rp.pointsFor', { place: t(`rp.place.${i + 1}` as Key) })}
                  value=${p.points[i]} min=${LIMITS.points.min} max=${LIMITS.points.max} onChange=${setPt} />
                ${rowChanged && html`<${ResetArrow} def=${String(d!.points[i])} onReset=${() => setPt(d!.points[i])} />`}
              </div>`;
            })}
          </div>
        <//>`}

        <p class="hint pe-track-hint">${t('rp.trackHint')} <a href="#/track-setup">${t('area.trackSetup')}</a></p>
        <p class="hint pe-track-hint">${t('rp.rulesHint')} <a href="#/tournaments">${t('area.tournaments')}</a></p>
      </div>

      <${Summary} p=${p} />
    </div>

    <${ConfirmDialog} open=${confirmReset}
      title=${t('rp.resetTitle', { name: presetName(p) })} body=${t('rp.resetBody')}
      confirmLabel=${t('rp.reset')} cancelLabel=${t('common.cancel')}
      onConfirm=${() => resetPreset(p.id)} onClose=${() => setConfirmReset(false)} />
    <${ConfirmDialog} open=${confirmSave}
      title=${t('rp.saveAsNewTitle')} body=${t('rp.saveAsNewBody', { name: presetName(p) })}
      confirmLabel=${t('rp.saveAsNew')} cancelLabel=${t('common.cancel')}
      onConfirm=${() => saveAsNew(p)} onClose=${() => setConfirmSave(false)} />
    <${ConfirmDialog} open=${confirmDelete} danger
      title=${t('rp.deleteTitle', { name: presetName(p) })} body=${t('rp.deleteBody')}
      confirmLabel=${t('rp.delete')} cancelLabel=${t('common.cancel')}
      onConfirm=${() => deletePreset(p.id)} onClose=${() => setConfirmDelete(false)} />
  </div>`;
}

// --- Summary card ---------------------------------------------------------------------

function Summary({ p }: { p: RacePreset }) {
  const lanes = trackLanes.value;
  const perRace = raceEstimateMs(p, avgLapMs.value, changeoverMs.value);
  const trackSetup = AREAS.find(a => a.id === 'track-setup')!;
  // A group only races on as many lanes as it has drivers, so its length depends on its size.
  // A tiebreaker is between 2, at most 3 drivers.
  const most = isTiebreaker(p.id) ? Math.min(3, lanes) : lanes;
  const sizes = Array.from({ length: most - LIMITS.drivers.min + 1 }, (_, i) => most - i);
  return html`<aside class="pe-summary">
    <${Panel} title=${t('rp.summary')}>
      <p class="sum-line">${presetSummary(p)}</p>
      <dl class="sum-stats">
        <div><dt>${t('rp.sum.warmup')}</dt><dd class="tnum">${p.warmupLaps}</dd></div>
        <div><dt>${t('rp.sum.perRace')}</dt><dd class="tnum">${formatDuration(perRace)}</dd></div>
      </dl>
      <table class="sum-table">
        <caption>${t(isTiebreaker(p.id) ? 'rp.sum.tableTitle.tb' : 'rp.sum.tableTitle')}</caption>
        <thead><tr><th scope="col">${t('rp.sum.drivers')}</th><th scope="col">${t('rp.sum.races')}</th><th scope="col">${t('rp.sum.total')}</th></tr></thead>
        <tbody>${sizes.map(n => html`<tr key=${n} class=${n === most ? 'full' : ''}>
          <th scope="row" class="tnum">${n}</th>
          <td class="tnum">${racesFor(p, n, lanes)}<small> = ${p.racesPerLane} × ${n}</small></td>
          <td class="tnum">${formatDuration(groupEstimateMs(p, n, avgLapMs.value, changeoverMs.value, lanes))}</td>
        </tr>`)}</tbody>
      </table>
      ${!isTiebreaker(p.id) && html`<p class="hint">${t('rp.sum.lanesNote', { k: p.racesPerLane, l: lanes })}</p>`}
      <p class="hint">${t('rp.sum.basis', {
        lap: (avgLapMs.value / 1000).toLocaleString(lang.value), change: (changeoverMs.value / 1000).toLocaleString(lang.value) })}
        ${' '}<a href=${'#/' + trackSetup.id}>${t('rp.sum.change')}</a></p>
    <//>
  </aside>`;
}

// --- Page ------------------------------------------------------------------------------

export function RacePresetsPage() {
  const a = AREAS.find(x => x.id === 'race-presets')!;
  const list = presets.value;
  const p = list.find(x => x.id === selectedPresetId.value) ?? list[0];
  return html`
    <${PageHeader} title=${t(a.title)} subtitle=${t(a.subtitle)} />
    ${!presetsPersistent.value && html`<p class="pe-note warn" role="note">${t('rp.notPersistent')}</p>`}
    <div class="md">
      <${PresetList} selected=${p.id} />
      ${presetsLoaded.value ? html`<${Editor} key=${p.id} p=${p} list=${list} />` : html`<div class="md-detail"></div>`}
    </div>`;
}
