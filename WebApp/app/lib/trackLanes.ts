// The track's lane count as app state (Track Setup → Track), stored in the
// library database as the setting "trackLanes". A connected time taker caps
// it: when it reports fewer lanes than set, the setting drops to fit (and is
// saved that way). Rules: src/track/lanes.ts.
import { signal, effect } from '@preact/signals';
import { DEFAULT_LANES, fitLanes, normalizeLanes, type LaneCount } from '../../src/track/lanes.ts';
import { openLibraryStorage } from '../../src/store/libraryStorage.ts';
import { trackLink } from '../shell/connection.ts';

const KEY = 'trackLanes';

export const trackLanes = signal<LaneCount>(DEFAULT_LANES);

const ready = openLibraryStorage().then(async s => {
  const stored = await s.getSetting(KEY);
  if (stored !== undefined) trackLanes.value = fitLanes(normalizeLanes(stored), trackLink.value?.lanes);
  return s;
}).catch(() => null);

export function setTrackLanes(n: LaneCount): void {
  const v = fitLanes(n, trackLink.value?.lanes);
  if (v === trackLanes.value) return;
  trackLanes.value = v;
  void ready.then(s => s?.putSetting(KEY, v)).catch(() => {});
}

// A time taker with fewer lanes connects: switch down to what it has.
effect(() => {
  const reported = trackLink.value?.lanes;
  const fitted = fitLanes(trackLanes.peek(), reported);
  if (fitted !== trackLanes.peek()) setTrackLanes(fitted);
});
