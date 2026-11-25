// Car Presets (GUI_SPEC §5a): master–detail like Race Presets. Left the
// presets in the host's order (drag and drop, Alt+arrows; "+ New car
// preset" at the end); right the selected preset's name and its cars on the
// lanes, the same board as in the Grand Prix's Cars step. Every change saves
// at once.
import { useEffect, useState } from 'preact/hooks';
import { html } from '../lib/html.ts';
import { t, type Key } from '../i18n/i18n.ts';
import {
  carPresets, carPresetsLoaded, carPresetsPersistent, selectedCarPresetId,
  createCarPreset, duplicateCarPreset, renameCarPreset, setPresetCar, deleteCarPreset, moveCarPreset,
} from '../lib/carPresets.ts';
import { CAR_PRESET_NAME_MAX, type CarPreset } from '../../src/grandPrix/cars.ts';
import { AREAS } from '../shell/areas.ts';
import { trackLanes } from '../lib/trackLanes.ts';
import { useSortable } from '../ui/sortable.ts';
import { CarBoard, CarChips } from '../ui/carBoard.ts';
import { PageHeader, Button, ConfirmDialog } from '../ui/ui.ts';

function PresetList({ selected }: { selected: string | null }) {
  const list = carPresets.value;
  const lanes = trackLanes.value;
  const sort = useSortable(list.length, moveCarPreset);
  const sortable = list.length > 1;
  const pick = (id: string) => () => { selectedCarPresetId.value = id; };
  return html`<nav class="md-list" aria-label=${t('cp.listLabel')}>
    <ul ref=${sort.listRef} class=${sort.dragging ? 'sorting' : ''}>
      ${list.map((p, i) => html`<li key=${p.id} ...${sortable ? sort.itemProps(i) : {}}>
        <button type="button" class=${'md-item' + (p.id === selected ? ' on' : '') + (sortable ? '' : ' fixed')}
            aria-current=${p.id === selected || undefined} aria-keyshortcuts=${sortable ? 'Alt+ArrowUp Alt+ArrowDown' : undefined}
            ...${sortable ? sort.controlProps(i, pick(p.id)) : { onClick: pick(p.id) }}>
          ${sortable && html`<span class="md-grip" data-drag-handle title=${t('rp.dragHint')} aria-hidden="true"></span>`}
          <span class="md-item-name">${p.name}</span>
          <${CarChips} cars=${p.cars} lanes=${lanes} />
        </button>
      </li>`)}
    </ul>
    <button type="button" class="md-add" onClick=${() => createCarPreset()}><span>+ ${t('cp.new')}</span></button>
    ${sortable && html`<p class="md-hint">${t('rp.dragHint')}</p>`}
    <p class="visually-hidden" aria-live="polite">${sort.lastMove ? t('rp.moved', { n: sort.lastMove.to + 1, total: sort.lastMove.count }) : ''}</p>
  </nav>`;
}

/** The name, edited in place; an empty or taken name goes back to the last good one. */
function NameField({ p }: { p: CarPreset }) {
  const [draft, setDraft] = useState(p.name);
  useEffect(() => setDraft(p.name), [p.id, p.name]);
  const name = draft.trim();
  const taken = carPresets.value.some(o => o.id !== p.id && o.name.toLocaleLowerCase() === name.toLocaleLowerCase());
  const problem: Key | null = !name ? 'rp.err.nameEmpty' : taken ? 'cp.err.nameTaken' : null;
  const commit = () => { if (problem || !renameCarPreset(p.id, name)) setDraft(p.name); };
  return html`<div class="pe-name">
    <div class="pe-ctl">
      <input class=${'pe-name-input' + (problem ? ' invalid' : '')} value=${draft} maxlength=${CAR_PRESET_NAME_MAX}
        aria-label=${t('cp.name')} aria-invalid=${!!problem} spellcheck="false"
        onInput=${(e: Event) => setDraft((e.target as HTMLInputElement).value)}
        onBlur=${commit}
        onKeyDown=${(e: KeyboardEvent) => {
          if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
          else if (e.key === 'Escape') setDraft(p.name);
        }} />
    </div>
    ${problem && html`<p class="field-error" role="alert">${t(problem)}</p>`}
  </div>`;
}

function Editor({ p }: { p: CarPreset }) {
  const [confirmDelete, setConfirmDelete] = useState(false);
  const lanes = trackLanes.value;
  return html`<div class="md-detail">
    <div class="pe-head">
      <${NameField} p=${p} />
      <div class="pe-actions">
        <${Button} onClick=${() => duplicateCarPreset(p)}>${t('rp.duplicate')}<//>
        <${Button} onClick=${() => setConfirmDelete(true)}>${t('rp.delete')}<//>
      </div>
    </div>
    <p class="pe-note">${t('cp.note')}</p>
    <${CarBoard} lanes=${lanes} cars=${p.cars} onChange=${(lane: number, patch: object) => setPresetCar(p.id, lane, patch)} />
    <p class="gp-side-lanes gpc-lanes">${t('tt.onLanes', { n: lanes })} <a href="#/track-setup">${t('tt.changeLanes')}</a></p>
    <${ConfirmDialog} open=${confirmDelete} danger
      title=${t('cp.deleteTitle', { name: p.name })} body=${t('cp.deleteBody')}
      confirmLabel=${t('rp.delete')} cancelLabel=${t('common.cancel')}
      onConfirm=${() => deleteCarPreset(p.id)} onClose=${() => setConfirmDelete(false)} />
  </div>`;
}

export function CarPresetsPage() {
  const a = AREAS.find(x => x.id === 'car-presets')!;
  const list = carPresets.value;
  const p = list.find(x => x.id === selectedCarPresetId.value) ?? list[0] ?? null;
  return html`
    <${PageHeader} title=${t(a.title)} subtitle=${t(a.subtitle)} />
    ${!carPresetsPersistent.value && html`<p class="pe-note warn" role="note">${t('rp.notPersistent')}</p>`}
    <div class="md">
      <${PresetList} selected=${p?.id ?? null} />
      ${!carPresetsLoaded.value ? html`<div class="md-detail"></div>`
        : p ? html`<${Editor} key=${p.id} p=${p} />`
        : html`<div class="md-detail cp-empty">
            <p class="gp-lead">${t('cp.empty')}</p>
            <div><${Button} kind="primary" onClick=${() => createCarPreset()}>+ ${t('cp.new')}<//></div>
          </div>`}
    </div>`;
}
