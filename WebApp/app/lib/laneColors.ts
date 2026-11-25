// The default lane colours, changeable in Settings (GUI_SPEC §11). Per
// browser. A lane set here overrides --lane-N on the root (in both themes);
// an unset lane keeps the theme's own default from styles.css. A Grand Prix
// still replaces them with its cars' colours.
import { signal, effect } from '@preact/signals';

export type LaneColors = readonly (string | null)[];   // index 0 = lane 1

const KEY = 'tt.laneColors';
const HEX = /^#[0-9a-f]{6}$/i;

function load(): LaneColors {
  try {
    const raw: unknown = JSON.parse(localStorage.getItem(KEY) ?? 'null');
    if (Array.isArray(raw)) return Array.from({ length: 6 }, (_, i) => (typeof raw[i] === 'string' && HEX.test(raw[i]) ? raw[i] : null));
  } catch { /* defaults */ }
  return Array(6).fill(null);
}

export const laneColors = signal<LaneColors>(load());

effect(() => {
  const c = laneColors.value;
  try { localStorage.setItem(KEY, JSON.stringify(c)); } catch { /* not persisted */ }
  const root = document.documentElement.style;
  c.forEach((v, i) => { if (v) root.setProperty(`--lane-${i + 1}`, v); else root.removeProperty(`--lane-${i + 1}`); });
});

/** null = back to the default. */
export function setLaneColor(lane: number, color: string | null): void {
  const next = laneColors.value.slice();
  next[lane - 1] = color && HEX.test(color) ? color.toLowerCase() : null;
  laneColors.value = next;
}

export function resetLaneColors(): void {
  laneColors.value = Array(6).fill(null);
}

/** The colour lane N shows now (#rrggbb), for the colour picker's start value. */
export function currentLaneColor(lane: number): string {
  const v = getComputedStyle(document.documentElement).getPropertyValue(`--lane-${lane}`).trim();
  return HEX.test(v) ? v.toLowerCase() : '#888888';
}

/** Dark or light text that reads on this colour. */
export function textOn(hex: string): string {
  const n = parseInt(hex.slice(1), 16);
  const lin = (c: number) => { const s = c / 255; return s <= 0.03928 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4; };
  const L = 0.2126 * lin(n >> 16 & 255) + 0.7152 * lin(n >> 8 & 255) + 0.0722 * lin(n & 255);
  return L > 0.2 ? '#111' : '#fff';
}
