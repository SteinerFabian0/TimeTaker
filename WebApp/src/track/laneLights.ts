/*
 * Lane lights before a race (GUI_SPEC §6.1b): the light above every lane
 * that has a car in the coming race is on, the others are off, so the
 * players see at a glance which seats are used. Pure, DOM-free; the app
 * sends the masks with DeviceApi.laneLed().
 */

/** Bit mask of lanes (lane 1 = bit 0). */
export function laneMask(lanes: Iterable<number>): number {
  let m = 0;
  for (const l of lanes) if (Number.isInteger(l) && l >= 1 && l <= 8) m |= 1 << (l - 1);
  return m;
}

/** The lights to switch for these active lanes on a track with `laneCount` lanes. */
export function laneLightMasks(active: readonly number[], laneCount: number): { on: number; off: number } {
  const all = laneMask(Array.from({ length: laneCount }, (_, i) => i + 1));
  const on = laneMask(active) & all;
  return { on, off: all & ~on };
}
