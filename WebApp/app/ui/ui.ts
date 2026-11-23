// Shared building blocks. Reused across areas (GUI_SPEC §12): the slanted
// look lives here and in styles.css, not in the pages.
import { html } from '../lib/html.ts';
import { t, type Key } from '../i18n/i18n.ts';
import { Icon } from '../shell/icons.ts';

export function PageHeader({ title, subtitle, children }: { title: string; subtitle?: string; children?: unknown }) {
  return html`<header class="page-header">
    <div>
      <h1>${title}</h1>
      ${subtitle && html`<p class="page-sub">${subtitle}</p>`}
    </div>
    ${children && html`<div class="page-actions">${children}</div>`}
  </header>`;
}

export function Panel({ title, tag, children, class: cls = '' }: { title?: string; tag?: string; children?: unknown; class?: string }) {
  return html`<section class=${'panel ' + cls}>
    ${title && html`<h2 class="panel-title"><span>${title}</span>${tag && html`<em class="tag">${tag}</em>`}</h2>`}
    <div class="panel-body">${children}</div>
  </section>`;
}

/** A slanted button. kind: 'primary' | 'ghost'. */
export function Button({ kind = 'ghost', disabled = false, onClick, children }:
  { kind?: 'primary' | 'ghost'; disabled?: boolean; onClick?: () => void; children?: unknown }) {
  return html`<button type="button" class=${'btn btn-' + kind} disabled=${disabled} onClick=${onClick}>
    <span>${children}</span></button>`;
}

/** A row of slanted options, one selected. */
export function Segmented<T extends string>({ value, options, onChange, disabled = false, big = false, label }:
  { value: T | null; options: { value: T; label: string }[]; onChange?: (v: T) => void; disabled?: boolean; big?: boolean; label: string }) {
  return html`<div class=${'seg' + (big ? ' seg-big' : '')} role="radiogroup" aria-label=${label}>
    ${options.map(o => html`<button type="button" role="radio" aria-checked=${o.value === value}
        class=${'seg-opt' + (o.value === value ? ' on' : '')} disabled=${disabled}
        onClick=${() => onChange?.(o.value)}><span>${o.label}</span></button>`)}
  </div>`;
}

export function PowerPackLock() {
  return html`<div class="pp-lock" role="note">
    <span class="pp-lock-icon"><${Icon} name="lock" size=${26} /></span>
    <div>
      <strong>${t('pp.lockTitle')}</strong>
      <p>${t('pp.lockBody')}</p>
    </div>
    <span class="tag tag-pp">${t('pp.badge')}</span>
  </div>`;
}

export function PlannedList({ keys }: { keys: Key[] }) {
  return html`<${Panel} title=${t('common.planned')} tag=${t('common.comingSoon')}>
    <ul class="planned">${keys.map(k => html`<li>${t(k)}</li>`)}</ul>
  <//>`;
}

/** Lap/race time: 27.384 (en) / 27,384 (de); minutes when ≥ 60 s. */
export function formatTime(ms: number, lang: string): string {
  const s = ms / 1000;
  const fmt = (v: number, int: number) => v.toLocaleString(lang, {
    minimumIntegerDigits: int, minimumFractionDigits: 3, maximumFractionDigits: 3 });
  if (s < 60) return fmt(s, 1);
  return `${Math.floor(s / 60)}:${fmt(s % 60, 2)}`;
}
