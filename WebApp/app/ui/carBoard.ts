// The cars on the lanes (GUI_SPEC §5a, §6.1a): a slot car from above
// (open- or closed-wheeler with its number plate) and the board that sets
// them up: per lane the car type, its colour, its number, and the lane
// itself live. Used by the Car Presets area and the Grand Prix's Cars step.
import { useEffect, useRef, useState } from 'preact/hooks';
import { html } from '../lib/html.ts';
import { t, type Key } from '../i18n/i18n.ts';
import { Select } from './ui.ts';
import {
  CAR_LABEL_MAX, CAR_PALETTE, CAR_TYPES, carIn, laneColorVars, typedLabel, type CarSet, type CarSetup, type CarType,
} from '../../src/grandPrix/cars.ts';

/** The plate with the car's text (its number or make), white with black letters so it reads on any colour. */
function Plate({ text, cx }: { text: string; cx: number }) {
  if (!text) return null;
  const n = [...text].length;
  const size = n <= 2 ? 17 : n === 3 ? 15 : 12.5;
  const w = n <= 2 ? 28 : n === 3 ? 37 : 40;
  return html`<g class="plate">
    <rect x=${cx - w / 2} y="16" width=${w} height="20" rx="4" />
    <text x=${cx} y="26.5" font-size=${size} text-anchor="middle" dominant-baseline="central">${text}</text>
  </g>`;
}

/**
 * A slot car from above, pointing right (towards the line); axles under
 * everything. Open-wheeler: wings, sidepods, the driver's helmet ahead of the
 * plate. Closed-wheeler: a GT car with its windows, the plate on the roof.
 */
export function Car({ type = 'open', label = '' }: { type?: CarType; label?: string }) {
  if (type === 'closed') return html`<svg class="rb-car-svg closed" viewBox="0 0 124 52" aria-hidden="true">
    <rect class="axle" x="27" y="6" width="4" height="40" /><rect class="axle" x="91" y="6" width="4" height="40" />
    <rect class="w" x="19" y="2" width="20" height="10" rx="3" /><rect class="w" x="19" y="40" width="20" height="10" rx="3" />
    <rect class="w" x="84" y="3" width="18" height="9" rx="3" /><rect class="w" x="84" y="40" width="18" height="9" rx="3" />
    <path class="body" d="M9 10 Q9 5 17 5 L98 5 Q115 6 119.5 17 L120 26 L119.5 35 Q115 46 98 47 L17 47 Q9 47 9 42 Z" />
    <path class="pod" d="M37 13 L72 12 Q76 26 72 40 L37 39 Q34 26 37 13 Z" />
    <path class="glass" d="M72 12 L83 15 Q86.5 26 83 37 L72 40 Q76 26 72 12 Z" />
    <path class="glass" d="M30 15 L37 13 Q34 26 37 39 L30 37 Q28 26 30 15 Z" />
    <path class="light" d="M110 9 L116 13 L113 15 Z" /><path class="light" d="M110 43 L116 39 L113 37 Z" />
    <rect class="wing" x="4" y="7" width="8" height="38" rx="2" />
    <${Plate} text=${label} cx=${54.5} />
  </svg>`;
  return html`<svg class="rb-car-svg" viewBox="0 0 124 52" aria-hidden="true">
    <rect class="axle" x="26" y="6" width="4" height="40" /><rect class="axle" x="90" y="8" width="4" height="36" />
    <rect class="w" x="18" y="2" width="20" height="11" rx="3" /><rect class="w" x="18" y="39" width="20" height="11" rx="3" />
    <rect class="w" x="84" y="5" width="16" height="9" rx="3" /><rect class="w" x="84" y="38" width="16" height="9" rx="3" />
    <rect class="wing" x="4" y="7" width="11" height="38" rx="2" />
    <path class="body" d="M12 20 L44 16 L62 14 L104 21 L118 24 L118 28 L104 31 L62 38 L44 36 L12 32 Z" />
    <rect class="pod" x="34" y="11" width="38" height="30" rx="8" />
    <rect class="wing" x="110" y="8" width="9" height="36" rx="2" />
    <ellipse class="helmet" cx=${label ? 80 : 58} cy="26" rx=${label ? 7 : 8} ry=${label ? 5.8 : 6.5} />
    <${Plate} text=${label} cx=${52} />
  </svg>`;
}

/**
 * The colour of a lane's car: a slanted swatch that opens a pop-up with the
 * usual car colours, a custom colour (the system picker) and the lane's
 * default from Settings. Escape or a click outside closes it.
 */
function ColorPick({ lane, color, set }: { lane: number; color: string | null; set: (patch: Partial<CarSetup>) => void }) {
  const [open, setOpen] = useState(false);
  const box = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!open) return;
    const away = (e: PointerEvent) => { if (!box.current?.contains(e.target as Node)) setOpen(false); };
    const esc = (e: KeyboardEvent) => { if (e.key === 'Escape') { setOpen(false); (box.current?.firstElementChild as HTMLElement)?.focus(); } };
    document.addEventListener('pointerdown', away);
    document.addEventListener('keydown', esc);
    return () => { document.removeEventListener('pointerdown', away); document.removeEventListener('keydown', esc); };
  }, [open]);
  const pick = (c: string | null) => { set({ color: c }); setOpen(false); };
  const label = t('gp.cars.colorOf', { lane });
  return html`<div class="gpc-color" ref=${box}>
    <button type="button" class="gpc-swatch" aria-haspopup="dialog" aria-expanded=${open} aria-label=${label} title=${label}
      onClick=${() => setOpen(o => !o)}><span></span></button>
    ${open && html`<div class="gpc-pop" role="dialog" aria-label=${label}>
      <div class="gpc-pal">
        ${CAR_PALETTE.map(c => html`<button type="button" class=${'gpc-chip' + (c === color ? ' on' : '')} style=${`--c: ${c}`}
          aria-label=${c} title=${c} aria-pressed=${c === color} onClick=${() => pick(c)}></button>`)}
      </div>
      <div class="gpc-pop-row">
        <label class="gpc-custom">
          <input type="color" value=${color ?? '#ff7a00'} aria-label=${t('gp.cars.custom')}
            onInput=${(e: Event) => set({ color: (e.target as HTMLInputElement).value })} />
          <span>${t('gp.cars.custom')}</span>
        </label>
        <button type="button" class=${'gpc-default' + (color === null ? ' on' : '')} style=${`--c: var(--lane-def-${lane})`}
          onClick=${() => pick(null)}><i></i><span>${t('gp.cars.default')}</span></button>
      </div>
    </div>`}
  </div>`;
}

/** The text on a lane's car (its number or make); every keystroke shows on the car at once. */
function CarLabel({ lane, label, set }: { lane: number; label: string; set: (patch: Partial<CarSetup>) => void }) {
  return html`<span class="gpc-label">
    <input type="text" value=${label} maxlength=${CAR_LABEL_MAX * 2} placeholder=${t('gp.cars.labelPh')}
      aria-label=${t('gp.cars.labelOf', { lane })} spellcheck="false" autocomplete="off"
      onInput=${(e: Event) => {
        const el = e.target as HTMLInputElement;
        const v = typedLabel(el.value);
        if (v !== el.value) el.value = v;
        set({ label: v });
      }} />
  </span>`;
}

/**
 * Per lane (1 at the top … `lanes`): the type dropdown, the colour, the
 * number, then the lane with its car as it will look in the races. The
 * board brings the cars' lane colours with it (`colors`) unless the page
 * around it already sets them (the Grand Prix).
 */
export function CarBoard({ lanes, cars, onChange, colors = true }:
  { lanes: number; cars: CarSet; onChange: (lane: number, patch: Partial<CarSetup>) => void; colors?: boolean }) {
  const types = CAR_TYPES.map(v => ({ value: v, label: t(`gp.cars.type.${v}` as Key) }));
  return html`<div class="gpc" style=${`--rows: ${lanes}; ${colors ? laneColorVars(cars) : ''}`}>
    <div class="gpc-cols" aria-hidden="true">
      <span>${t('gp.cars.type')}</span><span>${t('gp.cars.color')}</span><span>${t('gp.cars.label')}</span>
    </div>
    ${Array.from({ length: lanes }, (_, i) => {
      const l = i + 1;
      const c = carIn(cars, l);
      const set = (patch: Partial<CarSetup>) => onChange(l, patch);
      return html`<div key=${l} class=${'gpc-row' + (i === 0 ? ' first' : '') + (i === lanes - 1 ? ' last' : '')}
          style=${`--lane: var(--lane-${l}); --ink: var(--lane-${l}-ink, #111); --i: ${i}`}>
        <div class="gpc-ctl">
          <${Select} label=${t('gp.cars.typeOf', { lane: l })} value=${c.type} options=${types}
            onChange=${(v: CarType) => set({ type: v })} />
          <${ColorPick} lane=${l} color=${c.color} set=${set} />
          <${CarLabel} lane=${l} label=${c.label} set=${set} />
        </div>
        <div class="rb-lane">
          <span class="rb-road" aria-hidden="true"><span class="rb-slot"></span></span>
          <span class="rb-run"><span key=${c.type} class="rb-car gpc-car" role="img"
            aria-label=${t('gp.cars.carLabel', { lane: l, type: t(`gp.cars.type.${c.type}` as Key), label: c.label || '–' })}>
            <${Car} type=${c.type} label=${c.label} /></span></span>
        </div>
        <div class="rb-line"></div>
        <div class="rb-num tnum"><span>${l}</span></div>
      </div>`;
    })}
  </div>`;
}

/** A preset's cars in small: one slanted chip per lane in its car's colour (the lane default when it has none). */
export function CarChips({ cars, lanes }: { cars: CarSet; lanes: number }) {
  return html`<span class="car-chips" aria-hidden="true">${Array.from({ length: lanes }, (_, i) => {
    const c = carIn(cars, i + 1);
    return html`<i class=${c.type} style=${`--c: ${c.color ?? `var(--lane-def-${i + 1})`}`}></i>`;
  })}</span>`;
}
