// Area icons in currentColor. Glyphs: 24×24, square caps and miter joins
// (sharp, like the rest of the look); `d` is stroked, `fill` filled, `thin`
// stroked thin. Arts: filled artwork from the logo in its own coordinates.
import { html } from '../lib/html.ts';

interface Glyph { d: string; fill?: string; thin?: string }

/** Filled artwork: shapes in their own coordinates, optionally transformed. */
interface Art { viewBox: string; shapes: { d: string; transform?: string; evenodd?: boolean }[] }

// The logo's stopwatch (assets/logo.svg paths 1–10) without the speed lines:
// ring (even-odd), crown, side button, four ticks, hand, hub.
const stopwatch: Art = {
  viewBox: '-21.5 -44 43 43',
  shapes: [{ evenodd: true, d: 'M17 -18 C15.028 -8.611 5.819 -1 -3.57 -1 C-12.959 -1 -18.972 -8.611 -17 -18 C-15.028 -27.389 -5.819 -35 3.57 -35 C12.959 -35 18.972 -27.389 17 -18 Z M13.6 -18 C15.177 -25.511 10.367 -31.6 2.856 -31.6 C-4.655 -31.6 -12.023 -25.511 -13.6 -18 C-15.177 -10.489 -10.367 -4.4 -2.856 -4.4 C4.655 -4.4 12.023 -10.489 13.6 -18 Z' },
    { d: 'M0.96 -34 L5.76 -34 L6.852 -39.2 L2.052 -39.2 Z M-1.59 -39 L10.41 -39 L11.25 -43 L-0.75 -43 Z M12.717 -31.081 L16.14 -33.91 L18.598 -30.799 L15.175 -27.97 Z M9.969 -16.9 L13.969 -16.9 L14.431 -19.1 L10.431 -19.1 Z M-3.242 -7.8 L-4.082 -3.8 L-1.882 -3.8 L-1.042 -7.8 Z M-9.969 -19.1 L-13.969 -19.1 L-14.431 -16.9 L-10.431 -16.9 Z M3.242 -28.2 L4.082 -32.2 L1.882 -32.2 L1.042 -28.2 Z M0.642 -16.689 L8.456 -23.162 L-0.642 -19.311 Z M3 -18 C2.652 -16.343 1.027 -15 -0.63 -15 C-2.287 -15 -3.348 -16.343 -3 -18 C-2.652 -19.657 -1.027 -21 0.63 -21 C2.287 -21 3.348 -19.657 3 -18 Z' },
  ],
};

// Wrench crossed with a hammer. Each is drawn upright (head at the top,
// handle down, centred on x = 0), then rotated about the icon centre:
// hammer −45° (head top left), wrench +45° (open jaw top right).
const tools: Art = {
  viewBox: '0 0 24 24',
  shapes: [
    { transform: 'translate(12 12) rotate(-45) scale(.88)',      // hammer: handle, striking face, neck, claw
      d: 'M-1.4 -7.5H1.4V12.5H-1.4Z M-7.6 -13.6H-3V-12.3H2.6L7.6 -11.2 2.6 -8.7V-7.6H-3V-6.3H-7.6Z' },
    { transform: 'translate(12 12) rotate(45) scale(.88)',       // open-end wrench: handle, angled prongs, slot
      d: 'M-1.7 12.5V-6L-4.3 -7.6-4.8 -10.6-2.7 -14-1.5 -11.3V-9.3H1.5V-11.3L2.7 -14 4.8 -10.6 4.3 -7.6 1.7 -6V12.5Z' },
  ],
};

// Checkered flag: a pole and a 5 × 4 board (x 6…21, y 3…15) of 3-unit
// cells, every other one filled, inside a thin outline (a thick one would
// swallow the pattern).
function checkerCells(): string {
  let d = '';
  for (let row = 0; row < 4; row++)
    for (let col = 0; col < 5; col++)
      if ((row + col) % 2 === 0) d += `M${6 + col * 3} ${3 + row * 3}h3v3h-3z`;
  return d;
}

const glyphs: Record<string, Glyph> = {
  grandPrix: { d: 'M4 2v20', fill: checkerCells(), thin: 'M6 3h15v12H6z' },
  training:    // lap chart
    { d: 'M3 3v18h18 M6 16l4-6 4 3 5-8' },
  tournaments: // bracket
    { d: 'M3 4h5v6H3 M3 14h5v6H3 M8 7h4v10H8 M12 12h5 M17 9h4v6h-4z' },
  racePresets: // sliders
    { d: 'M4 6h10 M18 6h2 M14 4v4 M4 12h3 M11 12h9 M7 10v4 M4 18h12 M20 18h0 M16 16v4' },
  carPresets:  // a car from the side: body, two wheels
    { d: 'M2 16v-4l3-1.5 3.5-4h7l4 4 2.5 1.5v4h-2 M2 16h2 M10 16h4 M5 16a2 2 0 1 0 4 0a2 2 0 1 0 -4 0 M15 16a2 2 0 1 0 4 0a2 2 0 1 0 -4 0' },
  status:      // pulse
    { d: 'M2 12h4l3-7 4 14 3-7h6' },
  settings:    // gear: 8 square teeth + hub
    { d: 'M19.76 10.06L22.5 10.52v2.96l-2.74.46-.9 2.18 1.61 2.26-2.09 2.09-2.26-1.61-2.18.9-.46 2.74h-2.96' +
         'l-.46-2.74-2.18-.9-2.26 1.61-2.09-2.09 1.61-2.26-.9-2.18-2.74-.46v-2.96l2.74-.46.9-2.18-1.61-2.26' +
         ' 2.09-2.09 2.26 1.61 2.18-.9.46-2.74h2.96l.46 2.74 2.18.9 2.26-1.61 2.09 2.09-1.61 2.26z' +
         'M12 15.5a3.5 3.5 0 1 0 0-7 3.5 3.5 0 0 0 0 7z' },
  lock:
    { d: 'M6 11h12v10H6z M8 11V7a4 4 0 0 1 8 0v4' },
};

const arts: Record<string, Art> = { timeAttack: stopwatch, trackSetup: tools };

export function Icon({ name, size = 22 }: { name: string; size?: number }) {
  const art = arts[name];
  if (art) return html`<svg class="icon" width=${size} height=${size} viewBox=${art.viewBox} aria-hidden="true" fill="currentColor">
    ${art.shapes.map(sh => html`<path d=${sh.d} transform=${sh.transform} fill-rule=${sh.evenodd ? 'evenodd' : undefined} />`)}
  </svg>`;
  const g = glyphs[name] ?? glyphs.status;
  return html`<svg class="icon" width=${size} height=${size} viewBox="0 0 24 24" aria-hidden="true"
    fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="square" stroke-linejoin="miter">
    ${g.fill && html`<path d=${g.fill} fill="currentColor" stroke="none" />`}
    ${g.thin && html`<path d=${g.thin} stroke-width="1.25" />`}
    <path d=${g.d} />
  </svg>`;
}
