// The track's timing settings as app state (Track Setup → Start lights, Lap
// timing), loaded from the library database, saved on every change.
import { signal } from '@preact/signals';
import { DEFAULT_TRACK_TIMING, normalizeTrackTiming, type TrackTiming } from '../../src/track/trackTiming.ts';
import { openLibraryStorage } from '../../src/store/libraryStorage.ts';

const KEY = 'trackTiming';

export const trackTiming = signal<TrackTiming>(DEFAULT_TRACK_TIMING);

const ready = openLibraryStorage().then(async s => {
  const stored = await s.getSetting(KEY);
  if (stored !== undefined) trackTiming.value = normalizeTrackTiming(stored);
  return s;
}).catch(() => null);

export function updateTrackTiming(patch: Partial<TrackTiming>): void {
  const next = normalizeTrackTiming({ ...trackTiming.value, ...patch });
  trackTiming.value = next;
  void ready.then(s => s?.putSetting(KEY, next)).catch(() => {});
}

export function resetTrackTiming(): void {
  trackTiming.value = DEFAULT_TRACK_TIMING;
  void ready.then(s => s?.deleteSetting(KEY)).catch(() => {});
}
