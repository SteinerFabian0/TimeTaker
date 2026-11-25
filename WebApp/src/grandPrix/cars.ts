/*
 * The cars of a Grand Prix (GUI_SPEC §6.1a, step "Cars"). Pure functions, DOM-free.
 *
 * One car per lane for the whole evening. The host sets per lane the car's
 * type (open-wheeler or closed-wheeler), its colour (which becomes the lane
 * colour for this Grand Prix; null = the lane's default colour from
 * Settings) and a short text on it: its number or a make ("07", "BMW"), so
 * a car that came off goes back to its own lane.
 */

export const CAR_TYPES = ['open', 'closed'] as const;
export type CarType = typeof CAR_TYPES[number];

/** Letters or digits on the car, at most this many (it has to fit on the car's back). */
export const CAR_LABEL_MAX = 4;

export interface CarSetup {
  readonly type: CarType;
  /** '#rrggbb', or null for the lane's default colour. */
  readonly color: string | null;
  readonly label: string;
}

export const DEFAULT_CAR: CarSetup = { type: 'open', color: null, label: '' };

/** Lanes 1 … MAX_LANES can carry a car. */
export const MAX_LANES = 6;

/**
 * Colours that slot cars come in, for the picker (any other colour can still
 * be chosen with the custom field).
 */
export const CAR_PALETTE: readonly string[] = [
  '#d81e1e', '#8e1420', '#ff7a00', '#f5d02a',
  '#c9a23a', '#9bd12a', '#1f9d4a', '#0f5d34',
  '#14b3b3', '#3fb4ff', '#2357d9', '#8a4dff',
  '#ff5fa8', '#f2f2f2', '#a7adb5', '#1b1b1d',
];

/** What may stay in the text while typing: letters and digits, nothing else, at most CAR_LABEL_MAX. */
export function typedLabel(raw: string): string {
  return [...raw.replace(/[^\p{L}\p{N}]/gu, '')].slice(0, CAR_LABEL_MAX).join('');
}

/** A colour as kept: '#rrggbb' in lower case, or null. */
export function normalizeColor(v: unknown): string | null {
  if (typeof v !== 'string') return null;
  const s = v.trim().toLowerCase();
  if (/^#[0-9a-f]{6}$/.test(s)) return s;
  if (/^#[0-9a-f]{3}$/.test(s)) return '#' + [...s.slice(1)].map(c => c + c).join('');
  return null;
}

export function normalizeCar(v: unknown): CarSetup {
  const o = v && typeof v === 'object' ? v as Record<string, unknown> : {};
  return {
    type: CAR_TYPES.includes(o.type as CarType) ? o.type as CarType : DEFAULT_CAR.type,
    color: normalizeColor(o.color),
    label: typeof o.label === 'string' ? typedLabel(o.label) : '',
  };
}

/** The cars by lane ('1' … '6'), as stored; anything unreadable falls back to the default car. */
export function normalizeCars(v: unknown): Record<string, CarSetup> {
  const o = v && typeof v === 'object' && !Array.isArray(v) ? v as Record<string, unknown> : {};
  const out: Record<string, CarSetup> = {};
  for (let l = 1; l <= MAX_LANES; l++) {
    const c = normalizeCar(o[String(l)]);
    if (c.type !== DEFAULT_CAR.type || c.color !== null || c.label !== '') out[String(l)] = c;
  }
  return out;
}

/** WCAG relative luminance of '#rrggbb' (0 black … 1 white). */
export function luminance(hex: string): number {
  const ch = (i: number) => {
    const c = parseInt(hex.slice(i, i + 2), 16) / 255;
    return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * ch(1) + 0.7152 * ch(3) + 0.0722 * ch(5);
}

/**
 * Whether a lane colour is so dark or so light that, as text or a thin frame
 * on the page, it needs mixing with the text colour to be seen (a black car
 * on the dark theme, a white one on the light theme).
 */
export function needsEdge(hex: string): boolean {
  const L = luminance(hex);
  return L < 0.04 || L > 0.75;
}

/** Text on a background of this colour: dark on light colours, white on dark ones (whichever contrasts more). */
export function inkOn(hex: string): '#111111' | '#ffffff' {
  const L = luminance(hex);
  const dark = (L + 0.05) / (luminance('#111111') + 0.05);
  const light = 1.05 / (L + 0.05);
  return dark >= light ? '#111111' : '#ffffff';
}

/** The cars by lane, '1' … '6' (a missing lane has the default car). */
export type CarSet = Readonly<Record<string, CarSetup>>;

export const carIn = (cars: CarSet, lane: number): CarSetup => cars[String(lane)] ?? DEFAULT_CAR;

/** The same cars on lanes 1 … `lanes` (all six when not given). */
export function sameCars(a: CarSet, b: CarSet, lanes = MAX_LANES): boolean {
  for (let l = 1; l <= lanes; l++) {
    const x = carIn(a, l), y = carIn(b, l);
    if (x.type !== y.type || x.color !== y.color || x.label !== y.label) return false;
  }
  return true;
}

/** A set of cars with a name (Car Presets area), e.g. "Club GT cars". */
export interface CarPreset { readonly id: string; readonly name: string; readonly cars: CarSet }

export const CAR_PRESET_NAME_MAX = 40;

/** Stored car presets: unreadable ones are skipped, ids and names made unique. */
export function normalizeCarPresets(v: unknown): CarPreset[] {
  if (!Array.isArray(v)) return [];
  const out: CarPreset[] = [];
  for (const x of v) {
    if (!x || typeof x !== 'object') continue;
    const o = x as Record<string, unknown>;
    if (typeof o.id !== 'string' || !o.id || out.some(p => p.id === o.id)) continue;
    const name = typeof o.name === 'string' ? o.name.trim().slice(0, CAR_PRESET_NAME_MAX) : '';
    if (!name) continue;
    out.push({ id: o.id, name, cars: normalizeCars(o.cars) });
  }
  return out;
}

/**
 * The lane colours of a set of cars as CSS variables: a lane whose car has a
 * colour takes it (--lane-N), with a text colour that reads on it
 * (--lane-N-ink) and, for a very dark or light car, a version that shows as
 * text or a thin frame on the page (--lane-N-fg); other lanes keep the default.
 */
export function laneColorVars(cars: CarSet): string {
  let css = '';
  for (let l = 1; l <= MAX_LANES; l++) {
    const c = carIn(cars, l).color;
    if (!c) continue;
    css += `--lane-${l}: ${c}; --lane-${l}-ink: ${inkOn(c)}; `;
    if (needsEdge(c)) css += `--lane-${l}-fg: color-mix(in srgb, ${c} 58%, var(--text)); `;
  }
  return css;
}
