// Drag-and-drop reordering for a vertical list (race presets now, tournament
// templates later). Pointer events, so it works with a mouse, a pen and touch:
//   mouse   drag the item anywhere (it starts after a few pixels, so a plain
//           click still selects)
//   touch   drag the grip handle (the rest of the item scrolls the page)
//   keys    Alt+ArrowUp / Alt+ArrowDown on a focused item
// While dragging, the item follows the pointer and the others slide aside to
// show where it will land; nothing changes until it is dropped. Esc cancels.
import { useEffect, useRef, useState } from 'preact/hooks';
import { dropIndex } from '../../src/presets/order.ts';

/** Pixels the mouse must move before a press becomes a drag. */
const THRESHOLD = 5;

interface Gesture {
  from: number;
  startY: number;
  centres: number[];
  /** How far the others move to make room: the dragged item's height (with the shared border). */
  step: number;
  started: boolean;
  pointerId: number;
}

export interface Sortable {
  /** Put on the <ul> (or other container whose children are the items). */
  listRef: { current: HTMLElement | null };
  /** Spread onto each item's root element. */
  itemProps(i: number): Record<string, unknown>;
  /** Handlers for the item's focusable control (keyboard moves; swallows the click after a drag). */
  controlProps(i: number, onClick: () => void): Record<string, unknown>;
  dragging: boolean;
  /** For an aria-live region: "moved to 2 of 4" etc. (set by the caller's text). */
  lastMove: { to: number; count: number } | null;
}

/** handleOnly: only the grip starts a drag, also with a mouse (items holding text fields). */
export function useSortable(count: number, onMove: (from: number, to: number) => void, opts: { handleOnly?: boolean } = {}): Sortable {
  const listRef = useRef<HTMLElement | null>(null);
  const g = useRef<Gesture | null>(null);
  const swallowClick = useRef(false);
  const focusAfter = useRef<number | null>(null);
  const [view, setView] = useState<{ from: number; dy: number; to: number; step: number } | null>(null);
  const [lastMove, setLastMove] = useState<{ to: number; count: number } | null>(null);
  // The window listeners are created once (so they can be removed again);
  // they read the latest props and state through these refs.
  const latest = useRef({ count, onMove, view });
  latest.current = { count, onMove, view };

  const handlers = useRef<{ move: (e: PointerEvent) => void; up: (e: PointerEvent) => void; cancel: () => void; key: (e: KeyboardEvent) => void; end: (commit: boolean) => void } | null>(null);
  if (!handlers.current) {
    const end = (commit: boolean) => {
      const s = g.current;
      g.current = null;
      removeEventListener('pointermove', move);
      removeEventListener('pointerup', up);
      removeEventListener('pointercancel', cancel);
      removeEventListener('keydown', key, true);
      if (!s?.started) return;
      swallowClick.current = true;                      // the click that ends a drag is not a selection
      setTimeout(() => { swallowClick.current = false; }, 0);
      const v = latest.current.view;
      if (commit && v && v.to !== v.from) {
        latest.current.onMove(v.from, v.to);
        setLastMove({ to: v.to, count: latest.current.count });
      }
      setView(null);
    };
    const move = (e: PointerEvent) => {
      const s = g.current;
      if (!s || e.pointerId !== s.pointerId) return;
      const dy = e.clientY - s.startY;
      if (!s.started) {
        if (Math.abs(dy) < THRESHOLD) return;
        s.started = true;
      }
      e.preventDefault();
      const v = { from: s.from, dy, to: dropIndex(s.centres, s.from, s.centres[s.from] + dy), step: s.step };
      latest.current.view = v;
      setView(v);
    };
    const up = (e: PointerEvent) => { if (g.current && e.pointerId === g.current.pointerId) end(true); };
    const cancel = () => end(false);
    const key = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && g.current?.started) { e.preventDefault(); e.stopPropagation(); end(false); }
    };
    handlers.current = { move, up, cancel, key, end };
  }

  // Keep keyboard focus on the moved item after the list re-renders.
  useEffect(() => {
    if (focusAfter.current === null) return;
    const el = listRef.current?.children[focusAfter.current]?.querySelector<HTMLElement>('input, button, [tabindex]');
    el?.focus();
    focusAfter.current = null;
  });

  // Let go of the window listeners if the list goes away mid-drag.
  useEffect(() => () => handlers.current!.end(false), []);

  function onPointerDown(i: number, e: PointerEvent) {
    if (e.button !== 0 || count < 2 || !listRef.current || g.current) return;
    const onHandle = !!(e.target as Element).closest('[data-drag-handle]');
    if ((opts.handleOnly || e.pointerType !== 'mouse') && !onHandle) return;   // touch: only the handle drags, the rest scrolls
    const rects = [...listRef.current.children].map(el => el.getBoundingClientRect());
    const step = i < rects.length - 1 ? rects[i + 1].top - rects[i].top : rects[i].top - rects[i - 1].top;
    const started = onHandle && e.pointerType !== 'mouse';
    g.current = { from: i, startY: e.clientY, step, started, centres: rects.map(r => r.top + r.height / 2), pointerId: e.pointerId };
    if (started) {
      e.preventDefault();
      const v = { from: i, dy: 0, to: i, step };
      latest.current.view = v;
      setView(v);
    }
    const h = handlers.current!;
    addEventListener('pointermove', h.move, { passive: false });
    addEventListener('pointerup', h.up);
    addEventListener('pointercancel', h.cancel);
    addEventListener('keydown', h.key, true);
  }

  function offset(i: number): number {
    if (!view) return 0;
    const { from, to, dy, step } = view;
    if (i === from) return dy;
    if (from < to && i > from && i <= to) return -step;
    if (to < from && i >= to && i < from) return step;
    return 0;
  }

  return {
    listRef,
    dragging: view !== null,
    lastMove,
    itemProps: (i: number) => ({
      class: 'sortable-item' + (view?.from === i ? ' is-dragging' : ''),
      style: view ? `transform: translateY(${offset(i)}px)` : undefined,
      onPointerDown: (e: PointerEvent) => onPointerDown(i, e),
    }),
    controlProps: (i: number, onClick: () => void) => ({
      onClick: () => { if (!swallowClick.current) onClick(); },
      onKeyDown: (e: KeyboardEvent) => {
        if (!e.altKey || (e.key !== 'ArrowUp' && e.key !== 'ArrowDown')) return;
        e.preventDefault();
        const to = i + (e.key === 'ArrowUp' ? -1 : 1);
        if (to < 0 || to >= count) return;
        focusAfter.current = to;
        onMove(i, to);
        setLastMove({ to, count });
      },
    }),
  };
}
