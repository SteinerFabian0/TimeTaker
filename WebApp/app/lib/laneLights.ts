// The lane lights while a Grand Prix race page is open (GUI_SPEC §6.1b):
// the light above every lane with a car is on, the rest off, and it follows
// the host moving the empty lane. A page sets `wantedLaneLights`; null turns
// them all off again (once). Nothing is sent while a start sequence or a race
// session drives the lights, or without a time taker. Rules: src/track/laneLights.ts.
import { signal, effect } from '@preact/signals';
import { trackLink, raceSessionActive } from '../shell/connection.ts';
import { laneLightMasks } from '../../src/track/laneLights.ts';
import { LedMode } from '../../src/protocol/generated.ts';

/** The lanes whose lights should be on; null: none wanted (all off). */
export const wantedLaneLights = signal<readonly number[] | null>(null);
/** False when the last attempt to set them failed (shown on the page). */
export const laneLightsOk = signal(true);

let sent = '';            // what the track shows now, so nothing is sent twice

effect(() => {
  const link = trackLink.value;
  const want = wantedLaneLights.value;
  if (!link) { sent = ''; return; }
  if (raceSessionActive.value || link.seqRunning) return;
  if (want === null && sent === '') return;
  const { on, off } = laneLightMasks(want ?? [], link.lanes);
  const key = `${on}/${off}`;
  if (key === sent) return;
  sent = want === null ? '' : key;
  const api = link.api;
  Promise.all([
    on ? api.laneLed(on, { mode: LedMode.ON }) : Promise.resolve(),
    off ? api.laneLed(off, { mode: LedMode.OFF }) : Promise.resolve(),
  ]).then(() => { laneLightsOk.value = true; }, () => { laneLightsOk.value = false; sent = ''; });
});
