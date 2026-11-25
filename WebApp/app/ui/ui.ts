// Shared building blocks. Reused across areas (GUI_SPEC §12): the slanted
// look lives here and in styles.css, not in the pages.
import { useEffect, useRef, useState } from 'preact/hooks';
import { html } from '../lib/html.ts';
import { lang } from '../lib/prefs.ts';
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

/** A slanted button. kind: 'primary' | 'ghost' | 'danger'. */
export function Button({ kind = 'ghost', disabled = false, onClick, children, title, cls = '' }:
  { kind?: 'primary' | 'ghost' | 'danger'; disabled?: boolean; onClick?: () => void; children?: unknown; title?: string; cls?: string }) {
  return html`<button type="button" class=${'btn btn-' + kind + (cls ? ' ' + cls : '')} disabled=${disabled} onClick=${onClick}
    title=${title} aria-label=${title}>
    <span>${children}</span></button>`;
}

/** A row of slanted options, one selected. An option can be locked on its own (with a badge, e.g. Power Pack). */
export function Segmented<T extends string>({ value, options, onChange, disabled = false, big = false, label }:
  { value: T | null; options: { value: T; label: string; disabled?: boolean; badge?: string; title?: string }[];
    onChange?: (v: T) => void; disabled?: boolean; big?: boolean; label: string }) {
  return html`<div class=${'seg' + (big ? ' seg-big' : '')} role="radiogroup" aria-label=${label}>
    ${options.map(o => html`<button type="button" role="radio" aria-checked=${o.value === value}
        class=${'seg-opt' + (o.value === value ? ' on' : '') + (o.disabled ? ' locked' : '')}
        disabled=${disabled || o.disabled} title=${o.title}
        onClick=${() => onChange?.(o.value)}><span>${o.label}${o.badge && html` <em class="tag tag-pp">${o.badge}</em>`}</span></button>`)}
  </div>`;
}

/** A labelled setting: label (and hint) above its control. */
export function Field({ label, hint, children, id }: { label: string; hint?: unknown; children?: unknown; id?: string }) {
  return html`<div class="field">
    <label class="field-label" for=${id}>${label}</label>
    <div class="field-control">${children}</div>
    ${hint && html`<p class="field-hint">${hint}</p>`}
  </div>`;
}

/** A native dropdown in the slanted look (keyboard and touch come with the browser). */
/** A native dropdown; options with a `group` are shown under that heading (optgroup), in order of first appearance. */
export function Select<T extends string>({ value, options, onChange, label, id, disabled = false }:
  { value: T; options: { value: T; label: string; group?: string }[]; onChange?: (v: T) => void; label: string; id?: string; disabled?: boolean }) {
  const opt = (o: { value: T; label: string }) => html`<option value=${o.value} selected=${o.value === value}>${o.label}</option>`;
  const groups = [...new Set(options.map(o => o.group))];
  return html`<span class="select">
    <select id=${id} aria-label=${label} disabled=${disabled}
        onChange=${(e: Event) => onChange?.((e.target as HTMLSelectElement).value as T)}>
      ${groups.map(g => g === undefined ? options.filter(o => o.group === undefined).map(opt)
        : html`<optgroup label=${g}>${options.filter(o => o.group === g).map(opt)}</optgroup>`)}
    </select>
  </span>`;
}

const fmtNum =(v: number, decimals: number) =>
  v.toLocaleString(lang.value, { minimumFractionDigits: decimals, maximumFractionDigits: decimals, useGrouping: false });

/**
 * A number with − / + buttons (44 px touch targets) and a typed field.
 * The number is at least as wide as a button (a third of the stepper) and
 * grows with its digits; a unit adds its own width (GUI_SPEC §3.4).
 * Typing commits on Enter or leaving the field, clamped to min…max and
 * rounded to `step`; Escape reverts. Accepts "," as the decimal mark.
 */
export function Stepper({ value, min, max, step = 1, decimals = 0, unit, onChange, disabled = false, label, id }:
  { value: number; min: number; max: number; step?: number; decimals?: number; unit?: string;
    onChange?: (v: number) => void; disabled?: boolean; label: string; id?: string }) {
  const [draft, setDraft] = useState<string | null>(null);
  const clamp = (v: number) => {
    const r = Math.round(v / step) * step;
    return Number(Math.min(max, Math.max(min, r)).toFixed(decimals));
  };
  const commit = () => {
    if (draft === null) return;
    const v = Number(draft.replace(',', '.').trim());
    setDraft(null);
    if (draft.trim() !== '' && Number.isFinite(v) && clamp(v) !== value) onChange?.(clamp(v));
  };
  const bump = (d: number) => { setDraft(null); const v = clamp(value + d * step); if (v !== value) onChange?.(v); };
  // The field grows with what it shows (the CSS keeps it at least a button wide).
  const shown = draft ?? fmtNum(value, decimals);
  const chars = Math.max(1, shown.length);
  return html`<div class=${'stepper' + (disabled ? ' disabled' : '')} style=${`--chars: ${chars}`}>
    <button type="button" class="stepper-btn" aria-label=${'− ' + label} disabled=${disabled || value <= min}
      onClick=${() => bump(-1)}><span>−</span></button>
    <span class="stepper-field">
    <input id=${id} class="stepper-input tnum" type="text" inputmode=${decimals ? 'decimal' : 'numeric'}
      aria-label=${label} disabled=${disabled}
      value=${shown}
      onInput=${(e: Event) => setDraft((e.target as HTMLInputElement).value)}
      onBlur=${commit}
      onKeyDown=${(e: KeyboardEvent) => {
        if (e.key === 'Enter') commit();
        else if (e.key === 'Escape') setDraft(null);
        else if (e.key === 'ArrowUp') { e.preventDefault(); bump(1); }
        else if (e.key === 'ArrowDown') { e.preventDefault(); bump(-1); }
      }} />
    ${unit && html`<span class="stepper-unit">${unit}</span>`}
    </span>
    <button type="button" class="stepper-btn" aria-label=${'+ ' + label} disabled=${disabled || value >= max}
      onClick=${() => bump(1)}><span>+</span></button>
  </div>`;
}

/** A modal confirm (native <dialog>: focus trap, Esc closes). */
export function ConfirmDialog({ open, title, body, confirmLabel, cancelLabel, danger = false, onConfirm, onClose }:
  { open: boolean; title: string; body?: unknown; confirmLabel: string; cancelLabel: string; danger?: boolean;
    onConfirm: () => void; onClose: () => void }) {
  const ref = useRef<HTMLDialogElement>(null);
  useEffect(() => {
    const d = ref.current;
    if (!d) return;
    if (open && !d.open) d.showModal();
    else if (!open && d.open) d.close();
  }, [open]);
  return html`<dialog ref=${ref} class="dialog" onClose=${onClose}
      onClick=${(e: MouseEvent) => { if (e.target === ref.current) onClose(); }}>
    <div class="dialog-body">
      <h2 class="dialog-title">${title}</h2>
      ${body && html`<div class="dialog-text">${body}</div>`}
      <div class="dialog-actions">
        <${Button} onClick=${onClose}>${cancelLabel}<//>
        <button type="button" class=${'btn ' + (danger ? 'btn-danger' : 'btn-primary')}
          onClick=${() => { onConfirm(); onClose(); }}><span>${confirmLabel}</span></button>
      </div>
    </div>
  </dialog>`;
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

/** An estimate: "≈ 59 min", "≈ 1 h 05 min", "≈ 3 min 15 s" (short ones keep seconds). */
export function formatDuration(ms: number): string {
  const totalS = Math.round(ms / 1000);
  if (totalS < 600) {
    const m = Math.floor(totalS / 60), s = totalS % 60;
    return m ? (s ? `≈ ${m} min ${String(s).padStart(2, '0')} s` : `≈ ${m} min`) : `≈ ${s} s`;
  }
  const totalM = Math.round(ms / 60_000);
  const h = Math.floor(totalM / 60), m = totalM % 60;
  return h ? `≈ ${h} h ${String(m).padStart(2, '0')} min` : `≈ ${m} min`;
}
