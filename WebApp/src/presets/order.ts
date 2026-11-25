/*
 * The host's own order of a list (race presets now, templates later): a
 * stored list of ids. Pure functions, DOM-free.
 */

/**
 * `items` in the stored order. Items the order does not know yet (stored
 * before ordering existed, or the order was lost) follow in their given
 * order; ids in the order that no longer exist are ignored.
 */
export function applyOrder<T extends { id: string }>(items: readonly T[], order: readonly string[]): T[] {
  const rank = new Map(order.map((id, i) => [id, i]));
  const known = items.filter(x => rank.has(x.id)).sort((a, b) => rank.get(a.id)! - rank.get(b.id)!);
  return [...known, ...items.filter(x => !rank.has(x.id))];
}

/** The ids with the one at `from` moved to `to` (both clamped to the list). */
export function moveId(ids: readonly string[], from: number, to: number): string[] {
  const out = [...ids];
  if (from < 0 || from >= out.length) return out;
  const [x] = out.splice(from, 1);
  out.splice(Math.max(0, Math.min(out.length, to)), 0, x);
  return out;
}

/** The ids with `id` inserted right after `afterId` (at the end if it is missing). */
export function insertAfter(ids: readonly string[], id: string, afterId: string | null): string[] {
  const out = ids.filter(x => x !== id);
  const i = afterId === null ? -1 : out.indexOf(afterId);
  out.splice(i < 0 ? out.length : i + 1, 0, id);
  return out;
}

/**
 * Where a dragged item lands: its index among the others, from the centres
 * of all items (in list order) and where the dragged item's centre is now.
 */
export function dropIndex(centres: readonly number[], from: number, draggedCentre: number): number {
  let n = 0;
  centres.forEach((c, i) => { if (i !== from && c < draggedCentre) n++; });
  return n;
}
