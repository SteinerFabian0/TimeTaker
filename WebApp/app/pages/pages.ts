// Placeholder pages for the shell. Each shows what the area will hold; the
// Grand Prix, Track Setup and Settings pages also carry real-looking samples
// so the look (fonts, slant, orange, lane colours) can be judged.
import { useState } from 'preact/hooks';
import { html } from '../lib/html.ts';
import { t } from '../i18n/i18n.ts';
import { lang, theme, type Lang, type Theme } from '../lib/prefs.ts';
import { AREAS } from '../shell/areas.ts';
import { connection } from '../shell/connection.ts';
import { PageHeader, Panel, Button, Segmented, PowerPackLock, PlannedList, formatTime } from '../ui/ui.ts';

function header(id: string, children?: unknown) {
  const a = AREAS.find(x => x.id === id)!;
  return html`<${PageHeader} title=${t(a.title)} subtitle=${t(a.subtitle)}>${children}<//>`;
}

// --- Grand Prix ------------------------------------------------------------

const SAMPLE = [
  { lane: 1, name: 'Anna',   pos: 2, lap: 3, last: 26_412, best: 25_903, gap: 1_208 },
  { lane: 2, name: 'Ben',    pos: 1, lap: 3, last: 25_117, best: 24_880, gap: 0 },
  { lane: 3, name: 'Clara',  pos: 4, lap: 3, last: 28_731, best: 27_046, gap: 4_310 },
  { lane: 4, name: 'David',  pos: 3, lap: 3, last: 27_002, best: 26_118, gap: 2_954 },
  { lane: 5, name: 'Emil',   pos: 6, lap: 2, last: 31_560, best: 29_377, gap: -1 },
  { lane: 6, name: 'Fritzi', pos: 5, lap: 3, last: 27_915, best: 27_311, gap: 6_802 },
];

function LaneTile({ d, fastest }: { d: typeof SAMPLE[number]; fastest: boolean }) {
  const L = lang.value;
  return html`<article class="lane-tile" style=${`--lane: var(--lane-${d.lane})`}>
    <div class="lane-flag"><span>${d.lane}</span></div>
    <div class="lane-main">
      <div class="lane-top">
        <span class="lane-pos">P${d.pos}</span>
        <span class="lane-name">${d.name}</span>
        <span class="lane-lap"><small>${t('gp.lap')}</small> ${d.lap}<small>/5</small></span>
      </div>
      <dl class="lane-times">
        <div><dt>${t('gp.last')}</dt><dd class="time">${formatTime(d.last, L)}</dd></div>
        <div class=${fastest ? 'fastest' : ''}><dt>${t('gp.best')}</dt><dd class="time">${formatTime(d.best, L)}</dd></div>
        <div><dt>${t('gp.gap')}</dt><dd class="time">${d.gap === 0 ? '—' : d.gap < 0 ? '+1 L' : '+' + formatTime(d.gap, L)}</dd></div>
      </dl>
    </div>
  </article>`;
}

export function GrandPrixPage() {
  const fastest = Math.min(...SAMPLE.map(d => d.best));
  return html`
    ${header('grand-prix', html`<${Button} kind="primary">${t('gp.start')}<//>`)}
    <${Panel} title=${t('common.preview')} class="gp-preview">
      <div class="race-bar">
        <span class="chip chip-accent">${t('gp.groupA')}</span>
        <span class="race-count">${t('gp.raceOf', { n: 7, total: 18 })}</span>
        <span class="checker" aria-hidden="true"></span>
      </div>
      <div class="lane-grid">
        ${SAMPLE.map(d => html`<${LaneTile} key=${d.lane} d=${d} fastest=${d.best === fastest} />`)}
      </div>
    <//>
    <${PlannedList} keys=${['plan.grandPrix.1', 'plan.grandPrix.2', 'plan.grandPrix.3', 'plan.grandPrix.4']} />`;
}

// --- Simple placeholders ---------------------------------------------------

export const TimeAttackPage = () => html`${header('time-attack')}
  <${PlannedList} keys=${['plan.timeAttack.1', 'plan.timeAttack.2', 'plan.timeAttack.3']} />`;

export const TrainingPage = () => html`${header('training')}
  <${PlannedList} keys=${['plan.training.1', 'plan.training.2', 'plan.training.3']} />`;

export const TournamentsPage = () => html`${header('tournaments')}
  <${PlannedList} keys=${['plan.tournaments.1', 'plan.tournaments.2', 'plan.tournaments.3']} />`;

export const RacePresetsPage = () => html`${header('race-presets')}
  <${PlannedList} keys=${['plan.racePresets.1', 'plan.racePresets.2', 'plan.racePresets.3']} />`;

export function StatusPage() {
  const c = connection.value;
  return html`${header('status', html`<a class="btn btn-ghost" href="../debug/"><span>${t('status.debugLink')}</span></a>`)}
    <${Panel} title=${t('status.connection')}>
      <div class="conn-row">
        <span class=${'conn-dot ' + (c.link === 'offline' ? 'fail' : c.overall)}></span>
        <span>${t(`conn.${c.link}` as 'conn.offline')}</span>
        <span class=${'tag' + (c.powerPack ? ' tag-pp' : '')}>${c.powerPack ? t('conn.powerPack') : t('conn.noPowerPack')}</span>
      </div>
    <//>
    <${PlannedList} keys=${['plan.status.1', 'plan.status.2', 'plan.status.3']} />`;
}

// --- Track Setup -----------------------------------------------------------

export function TrackSetupPage() {
  const pp = connection.value.powerPack;
  // Expert mode is local state: off every time the page is opened (GUI_SPEC §8.2).
  const [expert, setExpert] = useState(false);
  const [level, setLevel] = useState<string>('3');
  return html`
    ${header('track-setup', html`<label class=${'switch' + (pp ? '' : ' disabled')}>
        <input type="checkbox" checked=${expert} disabled=${!pp}
          onChange=${(e: Event) => setExpert((e.target as HTMLInputElement).checked)} />
        <span class="switch-track"></span><span>${t('ts.expert')}</span></label>`)}
    <p class="hint expert-hint">${t('ts.expertHint')}</p>
    ${!pp && html`<${PowerPackLock} />`}
    <${Panel} title=${t('ts.speed')} class=${pp ? '' : 'locked'}>
      <${Segmented} big label=${t('ts.speed')} disabled=${!pp} value=${level} onChange=${setLevel}
        options=${[1, 2, 3, 4, 5].map(n => ({ value: String(n), label: String(n) }))} />
      <p class="hint">${t('ts.speedHint')}</p>
    <//>`;
}

// --- Settings --------------------------------------------------------------

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
        <div class="lane-swatches">
          ${[1, 2, 3, 4, 5, 6].map(n => html`<span class="swatch" style=${`--lane: var(--lane-${n})`}><span>${n}</span></span>`)}
        </div>
      <//>
      <${Panel} title=${t('set.about')}>
        <p class="hint">${t('set.version', { v: '0.1 (shell)' })}</p>
      <//>
    </div>`;
}

export const PAGES: Record<string, () => unknown> = {
  'grand-prix': GrandPrixPage,
  'time-attack': TimeAttackPage,
  'training': TrainingPage,
  'tournaments': TournamentsPage,
  'race-presets': RacePresetsPage,
  'track-setup': TrackSetupPage,
  'status': StatusPage,
  'settings': SettingsPage,
};
