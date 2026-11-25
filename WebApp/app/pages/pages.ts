// Placeholder pages for the shell and the page table. Each placeholder shows
// what the area will hold; Settings carries real-looking samples so the look
// (fonts, slant, orange, lane colours) can be judged.
import { html } from '../lib/html.ts';
import { t } from '../i18n/i18n.ts';
import { lang, theme, devTools, type Lang, type Theme } from '../lib/prefs.ts';
import { RacePresetsPage } from './racePresets.ts';
import { TrackSetupPage } from './trackSetup.ts';
import { TournamentsPage } from './tournaments.ts';
import { GrandPrixPage } from './grandPrix.ts';
import { CarPresetsPage } from './carPresets.ts';
import { AREAS } from '../shell/areas.ts';
import { StatusPage } from './status.ts';
import { PageHeader, Panel, Segmented, PlannedList, Button, ConfirmDialog } from '../ui/ui.ts';
import { laneColors, setLaneColor, resetLaneColors, currentLaneColor, textOn } from '../lib/laneColors.ts';
import { useRef, useState } from 'preact/hooks';
import { APP_VERSION } from '../lib/version.ts';
import { device, fwVersion, canUsb, canBle } from '../lib/device.ts';
import { PROTO } from '../../src/protocol/generated.ts';
import type { ImportPlan } from '../../src/store/libraryFile.ts';
import { storagePersisted, canKeepAwake, keptAwake } from '../lib/platform.ts';
import { libraryReady, exportLibrary, exportCounts, readImport, applyImport } from '../lib/library.ts';

function header(id: string, children?: unknown) {
  const a = AREAS.find(x => x.id === id)!;
  return html`<${PageHeader} title=${t(a.title)} subtitle=${t(a.subtitle)}>${children}<//>`;
}

// --- Simple placeholders ---------------------------------------------------

export const TimeAttackPage = () => html`${header('time-attack')}
  <${PlannedList} keys=${['plan.timeAttack.1', 'plan.timeAttack.2', 'plan.timeAttack.3']} />`;

export const TrainingPage = () => html`${header('training')}
  <${PlannedList} keys=${['plan.training.1', 'plan.training.2', 'plan.training.3']} />`;


// --- Settings --------------------------------------------------------------

/** The six lane colours: a click on a swatch opens the system colour picker. */
function LaneColorPicker() {
  void theme.value;                       // the defaults differ per theme
  const set = laneColors.value;
  return html`<div class="lane-swatches">
      ${[1, 2, 3, 4, 5, 6].map(n => {
        const c = currentLaneColor(n);
        return html`<label class=${'swatch swatch-pick' + (set[n - 1] ? ' changed' : '')} style=${`--lane: var(--lane-${n}); color: ${textOn(c)}`}
            title=${t('set.lanes.pick', { n })}>
          <span>${n}</span>
          <input type="color" value=${c} aria-label=${t('set.lanes.pick', { n })}
            onInput=${(e: Event) => setLaneColor(n, (e.target as HTMLInputElement).value)} />
        </label>`;
      })}
    </div>
    ${set.some(Boolean) && html`<div class="lane-reset"><${Button} onClick=${resetLaneColors}>${t('set.lanes.reset')}<//></div>`}`;
}


// --- Data: export / import ---------------------------------------------------

function DataPanel() {
  const input = useRef<HTMLInputElement>(null);
  const [plan, setPlan] = useState<ImportPlan | null>(null);
  const [note, setNote] = useState<{ text: string; warn?: boolean } | null>(null);
  const ready = libraryReady.value;
  const n = ready ? exportCounts() : null;

  const chosen = async (e: Event) => {
    const el = e.target as HTMLInputElement;
    const file = el.files?.[0];
    el.value = '';                           // the same file can be chosen again
    if (!file) return;
    setNote(null);
    const r = await readImport(file);
    if (!r.ok) { setNote({ text: t(`set.data.err.${r.error}`), warn: true }); return; }
    if (!r.plan.presets.length && !r.plan.templates.length && !r.plan.carPresets.length) { setNote({ text: t('set.data.nothing') }); return; }
    setPlan(r.plan);
  };
  const counts = (p: ImportPlan) => ({ p: p.presets.length, t: p.templates.length, c: p.carPresets.length });

  return html`<${Panel} title=${t('set.data')}>
    <p class="hint">${t('set.data.hint')}</p>
    <div class="data-actions">
      <${Button} kind="primary" disabled=${!ready} onClick=${exportLibrary}>${t('set.data.export')}<//>
      <${Button} disabled=${!ready} onClick=${() => input.current?.click()}>${t('set.data.import')}<//>
      <input ref=${input} type="file" accept=".json,application/json" hidden onChange=${chosen} />
    </div>
    ${n && html`<p class="hint">${t('set.data.exportCount', { p: n.presets, t: n.templates, c: n.carPresets })}</p>`}
    ${note && html`<p class=${note.warn ? 'field-warn' : 'hint'} role="status">${note.text}</p>`}
    <${ConfirmDialog} open=${plan !== null} title=${t('set.data.confirmTitle')}
      body=${plan && html`<p>${t('set.data.adds', counts(plan))}</p>
        ${plan.skipped > 0 && html`<p>${t('set.data.skipped', { n: plan.skipped })}</p>`}
        ${plan.renamed > 0 && html`<p>${t('set.data.renamed', { n: plan.renamed })}</p>`}`}
      confirmLabel=${t('set.data.confirm')} cancelLabel=${t('common.cancel')}
      onConfirm=${() => { if (plan) { applyImport(plan); setNote({ text: t('set.data.done', counts(plan)) }); } }}
      onClose=${() => setPlan(null)} />
  <//>`;
}

// --- About -------------------------------------------------------------------

function AboutPanel() {
  const d = device.value;
  const on = d.state === 'connected';
  const app = `${PROTO.major}.${PROTO.minor}`;
  const dev = on && d.hello ? `${d.hello.protoMajor}.${d.hello.protoMinor}` : null;
  const off = t('set.about.notConnected');
  const yesNo = (b: boolean) => t(b ? 'set.about.yes' : 'set.about.no');
  const rows: [string, string][] = [
    [t('set.about.app'), APP_VERSION],
    [t('set.about.protocol'), dev && dev !== app ? t('set.about.protocolDevice', { app, dev }) : app],
    [t('set.about.fwControl'), on ? fwVersion(d.nodes.ctrl) : off],
    [t('set.about.fwBle'), on ? fwVersion(d.nodes.ble) : off],
    [`${t('set.about.browser')} · ${t('set.about.usb')}`, yesNo(canUsb)],
    [`${t('set.about.browser')} · ${t('set.about.ble')}`, yesNo(canBle)],
    [t('set.about.storage'), t(storagePersisted.value === true ? 'set.about.storageKept' : storagePersisted.value === false ? 'set.about.storageMay' : 'set.about.unknown')],
    [t('set.about.awake'), !canKeepAwake ? t('set.about.no') : keptAwake.value ? t('set.about.awakeOn') : t('set.about.awakeReady')],
  ];
  return html`<${Panel} title=${t('set.about')}>
    <dl class="st-facts">
      ${rows.map(([k, v]) => html`<div><dt>${k}</dt><dd class="tnum">${v}</dd></div>`)}
    </dl>
  <//>`;
}

export function SettingsPage() {
  return html`
    ${header('settings')}
    <div class="settings-grid">
      <${Panel} title=${t('set.language')}>
        <${Segmented} label=${t('set.language')} value=${lang.value}
          onChange=${(v: Lang) => { lang.value = v; }}
          options=${[{ value: 'en', label: 'English' }, { value: 'de', label: 'Deutsch' }]} />
      <//>
      <${Panel} title=${t('set.theme')}>
        <${Segmented} label=${t('set.theme')} value=${theme.value}
          onChange=${(v: Theme) => { theme.value = v; }}
          options=${(['dark', 'light', 'system'] as const).map(v => ({ value: v, label: t(`set.theme.${v}`) }))} />
      <//>
      <${Panel} title=${t('set.lanes')}>
        <${LaneColorPicker} />
        <p class="hint">${t('set.lanes.hint')}</p>
      <//>
      <${Panel} title=${t('set.devTools')}>
        <${Segmented} label=${t('set.devTools')} value=${devTools.value ? 'on' : 'off'}
          onChange=${(v: string) => { devTools.value = v === 'on'; }}
          options=${[{ value: 'on', label: t('set.devTools.on') }, { value: 'off', label: t('set.devTools.off') }]} />
        <p class="hint">${t('set.devTools.hint')}</p>
      <//>
      <${DataPanel} />
      <${AboutPanel} />
    </div>`;
}

export const PAGES: Record<string, () => unknown> = {
  'grand-prix': GrandPrixPage,
  'time-attack': TimeAttackPage,
  'training': TrainingPage,
  'tournaments': TournamentsPage,
  'race-presets': RacePresetsPage,
  'car-presets': CarPresetsPage,
  'track-setup': TrackSetupPage,
  'status': StatusPage,
  'settings': SettingsPage,
};
