// Tournaments (GUI_SPEC §5). The tree is the page: the tournament for N
// drivers on the track's lanes (Track Setup) drawn left to right, Field →
// Groups → Second chance → Final, across the full width of the screen. Every
// card can be clicked; the settings of that kind of stage open below the
// tree, next to a plain explanation of what happens there. Tournaments to
// pick from sit as tabs above the tree. Every change saves at once. The plan
// itself is src/tournaments/plan.ts (DOM-free).
import { signal } from '@preact/signals';
import { useEffect, useLayoutEffect, useRef, useState } from 'preact/hooks';
import { html } from '../lib/html.ts';
import { t, type Key } from '../i18n/i18n.ts';
import { avgLapMs, changeoverMs, lang } from '../lib/prefs.ts';
import { presets, stagePresets, tiebreakerPreset, presetName, selectedPresetId } from '../lib/presets.ts';
import { trackLanes } from '../lib/trackLanes.ts';
import {
  templates, templatesLoaded, templatesPersistent, selectedTemplateId, templateName,
  createTemplate, duplicateTpl, updateTemplate, deleteTemplate, resetTemplate,
} from '../lib/tournaments.ts';
import {
  FORMATS, LIMITS, TIE_BREAKS, isTemplateAtDefaults,
  type Format, type TieBreak, type TournamentTemplate,
} from '../../src/tournaments/template.ts';
import {
  planTournament, planTotals, type PlanOptions, type PlanProblem, type Source, type Stage, type StageEstimate, type TournamentPlan,
} from '../../src/tournaments/plan.ts';
import { groupEstimateMs, racesFor, BUILTIN_IDS, type RacePreset } from '../../src/presets/preset.ts';
import { presetOption, presetSummary } from './racePresets.ts';
import { Button, Segmented, Field, Stepper, Select, ConfirmDialog, formatDuration } from '../ui/ui.ts';
import { connection } from '../shell/connection.ts';
import { RULE_LIMITS, type RaceRules } from '../../src/presets/rules.ts';

/** Drivers in the tree; kept while switching tournaments. */
const drivers = signal(12);

type Kind = 'field' | Stage['kind'];
/** The settings tab below the tree: a kind of card (selected in the tree too) or the tournament's rules. */
type Tab = Kind | 'rules';
const selectedKind = signal<Tab>('group');

// --- words -------------------------------------------------------------------------

const letter = (i: number) => String.fromCharCode(65 + i);

export function ordinal(n: number): string {
  if (lang.value === 'de') return `${n}.`;
  const s = n % 100 >= 11 && n % 100 <= 13 ? 'th' : ['th', 'st', 'nd', 'rd'][n % 10] ?? 'th';
  return n + s;
}

export function stageName(s: Stage): string {
  if (s.kind === 'group') return t('tt.group', { x: letter(s.index) });
  if (s.kind === 'repechage') return t('tt.second', { n: s.index + 1 });
  if (s.kind === 'smallFinal') return t('tt.smallFinal');
  return t('tt.final');
}

/** Where a group's drivers come from (seeds, a draw, everyone); places are shown as seats instead. */
function fromLines(plan: TournamentPlan, s: Stage, x: TournamentTemplate): string[] {
  const seeds = s.seats.filter((z): z is Extract<Source, { kind: 'seed' }> => z.kind === 'seed');
  if (!seeds.length) return [];
  if (x.split === 'random' && s.kind !== 'final') return [t('tt.from.random')];
  if (seeds.length === plan.drivers) return [t('tt.from.everyone')];
  return [t('tt.from.seeds', { list: seeds.map(z => z.seed).join(' · ') })];
}

/** "1 race" / "n races". */
export const racesText = (n: number): string => n === 1 ? t('tt.race1') : t('tt.races', { n });

export function sizesText(sizes: number[]): string {
  const lo = Math.min(...sizes), hi = Math.max(...sizes);
  return lo === hi ? t('tt.drivers', { n: lo }) : t('tt.driversRange', { lo, hi });
}

export function problemText(p: PlanProblem): string {
  switch (p.code) {
    case 'tooFewDrivers': return t('tpl.err.tooFewDrivers');
    case 'singleTooBig': return t('tpl.err.singleTooBig', { n: p.drivers, l: p.lanes });
    case 'tooManyGroups': return t('tpl.err.tooManyGroups', { g: p.groups, l: p.lanes });
    case 'noRepechage': return t('tpl.err.noRepechage', { g: p.groups, l: p.lanes });
    case 'repechageSeats': return t('tpl.err.repechageSeats', { wanted: p.wanted, used: p.used, valid: p.valid.join(', ') || '—' });
    case 'repechageTooBig': return t('tpl.err.repechageTooBig', { rest: p.rest, seats: p.seats });
    case 'stageTooBig': return t('tpl.err.stageTooBig', { stage: p.stage, n: p.size, l: p.lanes });
    case 'routedTwice': return t('tpl.err.routedTwice');
    case 'finalEmptySeats': return t('tpl.err.finalEmptySeats', { n: p.seats, l: p.lanes });
  }
}

// --- the planned tournament, with its numbers ------------------------------------------

export interface Planned {
  plan: TournamentPlan;
  lanes: number;
  byId: Map<string, RacePreset>;
  presetOf(s: Stage): RacePreset;
  est: Map<string, StageEstimate>;
  races: number;
  ms: number;
}

function usePlanned(x: TournamentTemplate): Planned {
  return plannedFor(x, drivers.value, { smallFinal: x.smallFinal });
}

/** The tournament for `n` drivers on the track's lanes, with races and estimated times. */
export function plannedFor(x: TournamentTemplate, n: number, opts: PlanOptions = {},
  adjust?: (s: Stage, p: RacePreset) => RacePreset): Planned {
  const lanes = trackLanes.value;
  const plan = planTournament(x, n, lanes, opts);
  const byId = new Map(presets.value.map(p => [p.id, p]));
  const base = (s: Stage) => byId.get(s.presetId) ?? byId.get(BUILTIN_IDS.default)!;
  /** The preset a stage races (with the Grand Prix's changes for the evening, if any). */
  const presetOf = (s: Stage) => adjust ? adjust(s, base(s)) : base(s);
  const tot = planTotals(plan, s => ({
    races: racesFor(presetOf(s), s.seats.length, lanes),
    ms: groupEstimateMs(presetOf(s), s.seats.length, avgLapMs.value, changeoverMs.value, lanes),
  }));
  return { plan, lanes, byId, presetOf, est: tot.perStage, races: tot.races, ms: tot.ms };
}

// --- the tree ----------------------------------------------------------------------------
//
// Node-graph style: plain rectangles. Every stage lists its places on its
// right edge (1 … size, as many as it has drivers), and every place that
// goes on has its own line, so each wire starts at the place it carries.
// A stage fed by places lists those seats on its left edge ("A3"), where
// the line ends. Places that skip a column (straight from the groups into
// the final) do not snake across: a short line ends in a small tag naming
// where they go, and the receiving card has the matching tag in front.

const HEAD_H = 40;          // column titles
const ROW = 26;             // one place / seat
const PAD_Y = 14;
const CONTENT_H = 110;      // title, stats, preset line (may wrap once)
const FROM_H = 24;          // the seeds line of a group, or "all drivers"
const NAME_H = 21;          // one driver's name (Grand Prix overview)
const GP_HEAD_H = 46;       // Grand Prix: title with its duration beside it, then drivers · races
const GP_CTL_H = 46;        // Grand Prix overview: the preset dropdown at the bottom of a card
const VGAP = 28;
const STUB = 16;            // the line in front of a shortcut tag

function useWidth<T extends HTMLElement>(): [{ current: T | null }, number] {
  const ref = useRef<T>(null);
  const [w, setW] = useState(0);
  useLayoutEffect(() => {
    const el = ref.current;
    if (!el) return;
    // Rounded down, with a pixel to spare: clientWidth rounds, and a canvas a
    // fraction of a pixel too wide shows a scrollbar with nothing to scroll.
    const inner = () => {
      const cs = getComputedStyle(el);
      return Math.floor(el.getBoundingClientRect().width - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight)) - 1;
    };
    setW(inner());
    const ro = new ResizeObserver(() => setW(inner()));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);
  return [ref, w];
}

interface Rect { x: number; y: number; w: number; h: number }
type RankSeat = Extract<Source, { kind: 'rank' }>;

/** Splits sorted numbers into runs of consecutive ones. */
function runs<T>(items: T[], key: (x: T) => number): T[][] {
  const out: T[][] = [];
  for (const it of items) {
    const last = out[out.length - 1];
    if (last && key(it) === key(last[last.length - 1]) + 1) last.push(it);
    else out.push([it]);
  }
  return out;
}

/**
 * The tree. With `names` (the drivers in seed order: names[0] is seed 1) it
 * is the Grand Prix overview: no drivers column, the first round's cards list
 * their drivers by name, and nothing can be selected.
 */
export function Tree({ x, pl, names, finalCtl, stageCtl, focus }: {
  x: TournamentTemplate; pl: Planned; names?: readonly string[];
  /**
   * Grand Prix phase page: only this column, sized to its content (not to the
   * space there is); every place leaving it and every seat entering it ends
   * in a tag naming the stage on the other side, as no other column is drawn.
   */
  focus?: number;
  /** Grand Prix overview: a control at the bottom of every stage card (the race preset dropdown), in place of the preset line. */
  stageCtl?: (s: Stage) => unknown;
  /** ↑ / ↓ right of the final and the small final: move a seat into the final, or down into the small final. */
  finalCtl?: { up: (() => void) | null; down: (() => void) | null };
}) {
  const [ref, width] = useWidth<HTMLDivElement>();
  const { plan } = pl;
  const sel: Tab | null = names ? null : selectedKind.value;
  const first = names ? 1 : 0;                      // the first column shown
  /** In the Grand Prix the final is the Grand Final. */
  const nm = (s: Stage) => names && (s.kind === 'final' || plan.groupIsFinal) ? t('gp.grandFinal') : stageName(s);

  const order = new Map(plan.stages.map((s, i) => [s.id, i]));
  const stageOf = (id: string) => plan.stages.find(s => s.id === id)!;
  const kindOf = (id: string): Kind => id === 'field' ? 'field' : stageOf(id).kind;

  // Columns: the field, then one per round.
  const ncol = Math.max(...plan.stages.map(s => s.column)) + 1;
  const ids: string[][] = Array.from({ length: ncol }, () => []);
  if (!names) ids[0].push('field');
  for (const s of plan.stages) ids[s.column].push(s.id);
  const focused = focus !== undefined;
  const shown = focused ? [focus] : ids.map((_, c) => c).filter(c => c >= first);

  // Seats fed by places (left strip): the ones arriving by shortcut tag on top, the wired
  // ones (from the column before) below them; each by source stage, then rank.
  const skips = (s: Stage, z: RankSeat) => stageOf(z.stage).column + 1 < s.column;
  /** Ends in a tag instead of a line: skips a column, or its other end is not drawn. */
  const tagged = (s: Stage, z: RankSeat) => focused || skips(s, z);
  const ins = new Map<string, RankSeat[]>(plan.stages.map(s => [s.id,
    s.seats.filter((z): z is RankSeat => z.kind === 'rank')
      .sort((a, b) => Number(skips(s, b)) - Number(skips(s, a)) || order.get(a.stage)! - order.get(b.stage)! || a.rank - b.rank)]));
  const dest = new Map<string, string>();           // "G1:3" → "R1"
  for (const s of plan.stages) for (const z of ins.get(s.id)!) dest.set(`${z.stage}:${z.rank}`, s.id);
  const outs = (s: Stage) => s.kind === 'final' || s.kind === 'smallFinal' ? 0 : s.seats.length;
  const seedLine = (s: Stage) => names || ins.get(s.id)!.length ? null : fromLines(plan, s, x)[0] ?? null;
  const namesOf = (s: Stage) => names ? s.seats.flatMap(z => z.kind === 'seed' ? [names[z.seed - 1] ?? '?'] : []) : [];

  // Room for shortcut tags: right of a column sending places on past the next one, and
  // in front of a column receiving them. Lines between neighbours run straight past it.
  const tagRoom = (text: string) => STUB + text.length * 6.5 + 22;
  const zoneOut = new Array<number>(ncol).fill(0), zoneIn = new Array<number>(ncol).fill(0);
  for (const s of plan.stages) for (const z of ins.get(s.id)!) if (tagged(s, z)) {
    const c = stageOf(z.stage).column;
    zoneOut[c] = Math.max(zoneOut[c], tagRoom(nm(s)));
    zoneIn[s.column] = Math.max(zoneIn[s.column], tagRoom(nm(stageOf(z.stage))));
  }

  // Widths from the space there is: the field narrower, generous gaps, capped cards.
  // The field only sends dashed lines on, so it and the gap after it stay narrow.
  const FOCUS_W = 260;                              // a card on its own (phase page)
  const W = focused ? zoneIn[focus] + FOCUS_W + zoneOut[focus] : Math.max(width, 760);
  const CTL_W = 64;                                 // room for the ↑ / ↓ right of the finals
  const Wl = finalCtl ? W - CTL_W : W;
  const fieldW = names ? 0 : Math.min(200, Math.max(110, W * 0.1));
  // Cards and gaps give way before the tree gets wider than the space (no
  // scrollbar for a few pixels): the gaps shrink down to 56 before the cards go below 230.
  const gapsFor = (gap: number) => {
    const g: number[] = [];                         // g[c]: right of column c
    for (let c = first; c < ncol - 1; c++) g[c] = c === 0 ? Math.max(52, gap * 0.6) : gap + (zoneOut[c] + zoneIn[c + 1]) / 2;
    return g;
  };
  const sum = (g: number[]) => g.reduce((a, b) => a + (b ?? 0), 0);
  const free = (g: number[]) => (Wl - fieldW - sum(g)) / (ncol - 1);
  let gaps = gapsFor(Math.min(190, Math.max(86, W * 0.08)));
  const MIN_UNIT = 230, MIN_GAP = 56;
  if (free(gaps) < MIN_UNIT) {
    const n = gaps.filter(g => g !== undefined).length || 1;
    const short = (MIN_UNIT - free(gaps)) * (ncol - 1) / n;
    gaps = gapsFor(Math.max(MIN_GAP, Math.min(190, Math.max(86, W * 0.08)) - short));
  }
  const sumGaps = sum(gaps);
  const unit = focused ? FOCUS_W : Math.min(340, Math.max(MIN_UNIT, free(gaps)));
  const colW = (c: number) => c === 0 ? fieldW : unit;
  const used = fieldW + sumGaps + (ncol - 1) * unit;
  const left = Math.max(0, (Wl - used) / 2);
  const colX = (c: number) => {
    if (focused) return zoneIn[focus];
    let x = left;
    for (let i = first; i < c; i++) x += colW(i) + gaps[i];
    return x;
  };

  // Heights: content or the longest strip, whichever is taller; one height per column.
  const needH = (id: string) => {
    if (id === 'field') return CONTENT_H + FROM_H + 2 * PAD_Y;
    const s = stageOf(id);
    const who = namesOf(s).length;
    const content = (names ? GP_HEAD_H + (stageCtl ? GP_CTL_H : 0) : CONTENT_H)
      + (seedLine(s) ? FROM_H : 0) + (who ? who * NAME_H + 18 : 0) + 2 * PAD_Y;
    return Math.max(content, Math.max(outs(s), ins.get(id)!.length) * ROW + 2 * PAD_Y);
  };
  const cardH = (c: number) => Math.max(...ids[c].map(needH));
  const colHeight = (c: number) => ids[c].length * cardH(c) + (ids[c].length - 1) * VGAP;
  const inner = Math.max(...shown.map(colHeight));
  const rect = new Map<string, Rect>();
  shown.forEach(c => {
    const y0 = HEAD_H + (inner - colHeight(c)) / 2;
    ids[c].forEach((id, i) => rect.set(id, { x: colX(c), y: y0 + i * (cardH(c) + VGAP), w: colW(c), h: cardH(c) }));
  });
  /** Centre y of row i (0-based) of a strip with n rows, centred on the card. */
  const rowY = (b: Rect, n: number, i: number) => b.y + (b.h - n * ROW) / 2 + i * ROW + ROW / 2;
  const outY = (id: string, rank: number) => rowY(rect.get(id)!, outs(stageOf(id)), rank - 1);
  const inY = (id: string, j: number) => rowY(rect.get(id)!, ins.get(id)!.length, j);

  // A column's only card that is fed by wires sits level with them, left to right: the
  // final's wired seats (at the bottom) face the second-chance winners, so those lines run
  // nearly flat, below the shortcut tags in front of the seats above.
  let bottom = HEAD_H + inner;
  for (const c of shown) {
    if (c === 0 || ids[c].length !== 1) continue;
    const id = ids[c][0];
    const s = stageOf(id);
    const wired = ins.get(id)!.map((z, j) => ({ z, j })).filter(({ z }) => !tagged(s, z));
    if (!wired.length) continue;
    const b = rect.get(id)!;
    const mean = (v: number[]) => v.reduce((a, q) => a + q, 0) / v.length;
    b.y = Math.max(HEAD_H, mean(wired.map(({ z }) => outY(z.stage, z.rank))) - mean(wired.map(({ j }) => inY(id, j) - b.y)));
    bottom = Math.max(bottom, b.y + b.h);
  }
  const H = bottom + 10;

  const colTitle = (c: number): string => {
    if (c === 0) return t('tt.col.field');
    const k = stageOf(ids[c][0]).kind;
    if (k === 'final') return t(ids[c].length > 1 ? 'tt.col.finals' : 'tt.col.final');
    return t('tt.col.round', { n: c, what: t(k === 'group' ? 'tt.col.groups' : 'tt.col.second') });
  };
  const seatCode = (z: RankSeat) => {
    const s = stageOf(z.stage);
    return s.kind === 'group' ? `${letter(s.index)}${z.rank}` : `${t('tt.code.second', { n: s.index + 1 })}·${z.rank}`;
  };
  const isOn = (a: string, b: string) => kindOf(a) === sel || kindOf(b) === sel;

  // Wires, port dots and shortcut tags.
  interface Wire { key: string; d: string; on: boolean; field?: boolean }
  interface Tag { key: string; x: number; y: number; h: number; text: string; on: boolean; side: 'out' | 'in' }
  const wires: Wire[] = [], tags: Tag[] = [];
  const dots: { x: number; y: number; on: boolean }[] = [];

  // The field feeds the first column as a whole.
  const fb = rect.get('field');
  if (fb) for (const id of ids[1] ?? []) {
    const b = rect.get(id)!;
    const x1 = fb.x + fb.w, y1 = fb.y + fb.h / 2, x2 = b.x, y2 = b.y + b.h / 2, m = (x1 + x2) / 2;
    wires.push({ key: 'field>' + id, d: `M${x1} ${y1} C${m} ${y1}, ${m} ${y2}, ${x2} ${y2}`, on: isOn('field', id), field: true });
  }

  for (const s of plan.stages) {
    const list = ins.get(s.id)!;
    const b = rect.get(s.id);
    // Direct: from a place of the previous column to a seat here.
    if (b) list.forEach((z, j) => {
      if (tagged(s, z)) return;
      const a = rect.get(z.stage)!;
      const x1 = a.x + a.w, y1 = outY(z.stage, z.rank), x2 = b.x, y2 = inY(s.id, j);
      const p = x1 + zoneOut[s.column - 1], q = x2 - zoneIn[s.column], m = (p + q) / 2;
      const on = isOn(z.stage, s.id);
      wires.push({ key: `${z.stage}:${z.rank}>${s.id}`, d: `M${x1} ${y1} H${p} C${m} ${y1}, ${m} ${y2}, ${q} ${y2} H${x2}`, on });
      dots.push({ x: x1, y: y1, on }, { x: x2, y: y2, on });
    });
    // Shortcut: places skipping a column end in a tag; the seats here start at the matching tag.
    const skipping = list.map((z, j) => ({ z, j })).filter(({ z }) => tagged(s, z));
    const bySrc = new Map<string, { z: RankSeat; j: number }[]>();
    for (const e of skipping) bySrc.set(e.z.stage, [...(bySrc.get(e.z.stage) ?? []), e]);
    for (const [src, group] of bySrc) {
      const a = rect.get(src);
      const on = isOn(src, s.id);
      if (a) for (const run of runs(group, e => e.z.rank)) {
        const y1 = outY(src, run[0].z.rank) - ROW / 2 + 3, y2 = outY(src, run[run.length - 1].z.rank) + ROW / 2 - 3;
        tags.push({ key: `o${src}>${s.id}:${run[0].z.rank}`, x: a.x + a.w + STUB, y: y1, h: y2 - y1, text: nm(s), on, side: 'out' });
        for (const { z } of run) {
          const y = outY(src, z.rank);
          wires.push({ key: `${src}:${z.rank}>tag`, d: `M${a.x + a.w} ${y} H${a.x + a.w + STUB}`, on });
          dots.push({ x: a.x + a.w, y, on });
        }
      }
      if (b) for (const run of runs(group, e => e.j)) {
        const y1 = inY(s.id, run[0].j) - ROW / 2 + 3, y2 = inY(s.id, run[run.length - 1].j) + ROW / 2 - 3;
        tags.push({ key: `i${src}>${s.id}:${run[0].j}`, x: b.x - STUB, y: y1, h: y2 - y1, text: nm(stageOf(src)), on, side: 'in' });
        for (const { j } of run) {
          const y = inY(s.id, j);
          wires.push({ key: `tag>${s.id}:${j}`, d: `M${b.x - STUB} ${y} H${b.x}`, on });
          dots.push({ x: b.x, y, on });
        }
      }
    }
  }
  // Selected wires drawn last, so they lie on top.
  wires.sort((p, q) => Number(p.on) - Number(q.on));

  const card = (id: string) => {
    const b = rect.get(id)!;
    const k = kindOf(id);
    const style = `left:${b.x}px; top:${b.y}px; width:${b.w}px; height:${b.h}px`;
    const pick = () => { if (!names) selectedKind.value = k; };
    if (id === 'field') return html`<button type="button" key="field" class=${`tt-card tt-field${k === sel ? ' sel' : ''}`}
        style=${style} onClick=${pick} aria-pressed=${k === sel}>
      <span class="tt-big tnum">${plan.drivers}</span>
      <span class="tt-sub">${t('tt.driversWord')}</span>
      ${plan.stages.some(st => st.kind === 'group')
        && html`<span class="tt-meta">${t(x.split === 'snake' ? 'tt.split.snake.short' : 'tt.split.random.short')}</span>`}
    </button>`;
    const s = stageOf(id);
    const e = pl.est.get(id)!;
    const p = pl.byId.get(s.presetId);
    const seats = ins.get(id)!;
    const n = outs(s);
    const seed = seedLine(s);
    const cls = `tt-card tt-${k}` + (k === sel ? ' sel' : '') + (seats.length ? ' has-in' : '') + (n ? ' has-out' : '');
    // Nothing to select in the Grand Prix, so there the card is not a button.
    const Tag = names ? 'div' : 'button';
    const btn = names ? {} : { type: 'button', onClick: pick, 'aria-pressed': k === sel };
    return html`<${Tag} key=${id} class=${cls} style=${style} ...${btn}>
      ${names
        ? html`<span class="tt-card-head">
            <span class="tt-title">${plan.groupIsFinal || s.kind === 'final' ? (names ? t('gp.grandFinal') : t('tt.final')) : nm(s)}</span>
            <span class="tt-meta tnum">${formatDuration(e.ms)}</span>
          </span>
          <span class="tt-stats tnum">${t('tt.drivers', { n: s.seats.length })} · ${racesText(e.races)}</span>`
        : html`<span class="tt-title">${plan.groupIsFinal || s.kind === 'final' ? (names ? t('gp.grandFinal') : t('tt.final')) : nm(s)}</span>
          <span class="tt-stats tnum">${t('tt.drivers', { n: s.seats.length })} · ${racesText(e.races)}</span>
          <span class="tt-meta">${p ? presetName(p) : t('tt.presetMissing')} · <span class="tnum">${formatDuration(e.ms)}</span></span>`}
      ${seed && html`<span class="tt-seeds">${seed}</span>`}
      ${namesOf(s).length > 0 && html`<span class="tt-names">${namesOf(s).map(nm => html`<span>${nm}</span>`)}</span>`}
      ${seats.length > 0 && html`<span class="tt-strip tt-in" aria-hidden="true">
        ${seats.map((z, j) => html`<span class="tt-seat tnum" style=${`top:${inY(id, j) - b.y - ROW / 2}px`}
          title=${t('tt.from.places', { places: ordinal(z.rank), stage: nm(stageOf(z.stage)) })}>${seatCode(z)}</span>`)}
      </span>`}
      ${n > 0 && html`<span class="tt-strip tt-out">
        <span class="tt-strip-label" aria-hidden="true">${t('tt.places')}</span>
        ${Array.from({ length: n }, (_, i) => {
          const to = dest.get(`${id}:${i + 1}`);
          return html`<span class=${'tt-place tnum' + (to ? '' : ' gone')} style=${`top:${outY(id, i + 1) - b.y - ROW / 2}px`}
            title=${to ? `${ordinal(i + 1)} → ${nm(stageOf(to))}` : t('tt.place.out', { place: ordinal(i + 1) })}>${i + 1}.</span>`;
        })}
      </span>`}
      ${stageCtl && html`<div class="tt-ctl">${stageCtl(s)}</div>`}
    <//>`;
  };

  return html`<div class=${'tt-tree' + (names ? ' tt-static' : '') + (focused ? ' tt-focus' : '')} ref=${ref}>
    ${(width > 0 || focused) && html`<div class="tt-canvas" style=${`width:${W}px; height:${H}px`}>
      ${shown.map(c => html`<div class="tt-col-title" style=${`left:${colX(c)}px; width:${colW(c)}px`}>${colTitle(c)}</div>`)}
      <svg class="tt-wires" width=${W} height=${H} viewBox=${`0 0 ${W} ${H}`} aria-hidden="true">
        ${wires.map(w => html`<path key=${w.key} d=${w.d} class=${'tt-wire' + (w.on ? ' on' : '') + (w.field ? ' field' : '')} />`)}
      </svg>
      ${shown.flatMap(c => ids[c]).map(card)}
      <svg class="tt-dots" width=${W} height=${H} viewBox=${`0 0 ${W} ${H}`} aria-hidden="true">
        ${dots.map(d => html`<circle cx=${d.x} cy=${d.y} r="3.5" class=${d.on ? 'on' : ''} />`)}
      </svg>
      ${tags.map(g => html`<span key=${g.key} class=${`tt-tag tt-tag-${g.side}` + (g.on ? ' on' : '')} aria-hidden="true"
        style=${`left:${g.x}px; top:${g.y}px; height:${g.h}px`}>${g.text}</span>`)}
      ${finalCtl && rect.has('F') && rect.has('K') && (() => {
        const f = rect.get('F')!, k = rect.get('K')!;
        return html`<div class="tt-fctl" style=${`left:${f.x + f.w + 16}px; top:${(f.y + f.h + k.y) / 2}px`}>
          <button type="button" class="tt-fctl-btn" disabled=${!finalCtl.up} onClick=${() => finalCtl.up?.()}
            title=${t('gp.seatUp')} aria-label=${t('gp.seatUp')}>▲</button>
          <button type="button" class="tt-fctl-btn" disabled=${!finalCtl.down} onClick=${() => finalCtl.down?.()}
            title=${t('gp.seatDown')} aria-label=${t('gp.seatDown')}>▼</button>
        </div>`;
      })()}
    </div>`}
  </div>`;
}

// --- the frame around the tree: name, format, actions; drivers and totals -----------------

function NameField({ x, list }: { x: TournamentTemplate; list: TournamentTemplate[] }) {
  const shown = templateName(x);
  const [draft, setDraft] = useState(shown);
  useEffect(() => setDraft(shown), [x.id, shown]);
  const name = draft.trim();
  const taken = list.some(o => o.id !== x.id && [o.name, templateName(o)].some(n => n.trim().toLocaleLowerCase() === name.toLocaleLowerCase()));
  const problem: Key | null = !name ? 'rp.err.nameEmpty' : taken ? 'tpl.err.nameTaken' : null;
  const commit = () => {
    if (problem) setDraft(shown);
    else if (name !== shown) updateTemplate(x.id, { name });
  };
  return html`<div class="tt-name">
    <input class=${'tt-name-input' + (problem ? ' invalid' : '')} value=${draft} maxlength="48"
      aria-label=${t('tpl.name')} aria-invalid=${!!problem} spellcheck="false" title=${t('tt.rename')}
      onInput=${(e: Event) => setDraft((e.target as HTMLInputElement).value)}
      onBlur=${commit}
      onKeyDown=${(e: KeyboardEvent) => {
        if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
        else if (e.key === 'Escape') setDraft(shown);
      }} />
    ${problem && html`<p class="field-error" role="alert">${t(problem)}</p>`}
  </div>`;
}

function TreeFrame({ x, list, pl }: { x: TournamentTemplate; list: TournamentTemplate[]; pl: Planned }) {
  const [confirmDelete, setConfirmDelete] = useState(false);
  const [confirmReset, setConfirmReset] = useState(false);
  const atDefaults = x.builtIn && isTemplateAtDefaults(x);
  const { plan } = pl;
  const errors = plan.problems.filter(p => p.level === 'error');
  const warns = plan.problems.filter(p => p.level === 'warn');

  return html`<section class="tt-stage" aria-label=${templateName(x)}>
    <div class="tt-bar">
      <div class="tt-bar-main">
        <${NameField} x=${x} list=${list} />
        ${x.builtIn
          ? html`<span class="tt-format-fixed" title=${t('tt.format.fixed')}>
              ${templateName(x) !== t(`tpl.format.${x.format}` as Key) && t(`tpl.format.${x.format}` as Key)}
              <em class="md-item-tag">${t('rp.builtInTag')}</em></span>`
          : html`<${Segmented} label=${t('tpl.format')} value=${x.format} onChange=${(format: Format) => updateTemplate(x.id, { format })}
              options=${FORMATS.map(f => ({ value: f, label: t(`tpl.format.${f}` as Key) }))} />`}
      </div>
      <div class="tt-bar-actions">
        <${Button} onClick=${() => duplicateTpl(x)}>${t('rp.duplicate')}<//>
        ${x.builtIn
          ? html`<${Button} disabled=${atDefaults} onClick=${() => setConfirmReset(true)}>${t('rp.reset')}<//>`
          : html`<${Button} onClick=${() => setConfirmDelete(true)}>${t('rp.delete')}<//>`}
      </div>
    </div>

    ${errors.length
      ? html`<div class="tt-empty" role="alert">
          <p class="tt-empty-title">${t('tt.cannot', { n: plan.drivers, l: pl.lanes })}</p>
          ${errors.map(p => html`<p>${problemText(p)}</p>`)}
        </div>`
      : html`<${Tree} x=${x} pl=${pl} />`}

    <div class="tt-foot">
      <div class="tt-foot-field">
        <span class="tt-foot-label" id="tt-n-label">${t('tt.driversQ')}</span>
        <${Stepper} id="tt-n" label=${t('tt.driversQ')} value=${drivers.value} min=${LIMITS.drivers.min} max=${LIMITS.drivers.max}
          onChange=${(v: number) => { drivers.value = v; }} />
        <span class="tt-foot-lanes">${t('tt.onLanes', { n: pl.lanes })} <a href="#/track-setup">${t('tt.changeLanes')}</a></span>
      </div>
      ${warns.map(p => html`<p class="tt-warn" role="note">${problemText(p)}</p>`)}
      ${!errors.length && html`<dl class="tt-totals">
        <div><dt>${t('tt.tot.races')}</dt><dd class="tnum">${pl.races}</dd></div>
        <div class="tt-total"><dt>${t('tt.tot.duration')}</dt><dd class="tnum">${formatDuration(pl.ms)}</dd></div>
      </dl>`}
    </div>

    <${ConfirmDialog} open=${confirmReset}
      title=${t('rp.resetTitle', { name: templateName(x) })} body=${t('tpl.resetBody')}
      confirmLabel=${t('rp.reset')} cancelLabel=${t('common.cancel')}
      onConfirm=${() => resetTemplate(x.id)} onClose=${() => setConfirmReset(false)} />
    <${ConfirmDialog} open=${confirmDelete} danger
      title=${t('rp.deleteTitle', { name: templateName(x) })} body=${t('tpl.deleteBody')}
      confirmLabel=${t('rp.delete')} cancelLabel=${t('common.cancel')}
      onConfirm=${() => deleteTemplate(x.id)} onClose=${() => setConfirmDelete(false)} />
  </section>`;
}

// --- settings of the selected kind of stage --------------------------------------------------

function PresetPick({ id, label, value, onChange }: { id: string; label: string; value: string; onChange: (v: string) => void }) {
  const list = stagePresets.value;
  const p = presets.value.find(q => q.id === value);
  return html`<${Field} label=${label} id=${id}
      hint=${p ? html`${presetSummary(p)} · <a href="#/race-presets">${t('tt.editPresets')}</a>`
        : html`<span class="field-error">${t('tpl.presetMissing')}</span>`}>
    <${Select} id=${id} label=${label} value=${value} onChange=${onChange}
      options=${[...(p ? (list.includes(p) ? [] : [presetOption(p)]) : [{ value, label: '—' }]), ...list.map(presetOption)]} />
  <//>`;
}

function TieField({ x }: { x: TournamentTemplate }) {
  return html`<div class="tt-setting">
    <${Field} label=${t('tpl.tie')} hint=${t(`tpl.tie.${x.tieBreak}.hint` as Key)}>
      <${Segmented} label=${t('tpl.tie')} value=${x.tieBreak} onChange=${(tieBreak: TieBreak) => updateTemplate(x.id, { tieBreak })}
        options=${TIE_BREAKS.map(v => ({ value: v, label: t(`tpl.tie.${v}` as Key) }))} />
    <//>
    ${x.tieBreak === 'tiebreakerRace' && html`<p class="tt-note">${t('tpl.tiebreakerUses', { summary: presetSummary(tiebreakerPreset.value) })}
      ${' '}<a href="#/race-presets" onClick=${() => { selectedPresetId.value = tiebreakerPreset.value.id; }}>${t('tt.editPresets')}</a></p>`}
    <p class="tt-note">${t('tt.tieShared')}</p>
  </div>`;
}

/** What happens at this kind of stage, in sentences, from the plan. */
function explain(kind: Tab, x: TournamentTemplate, pl: Planned): string[] {
  const { plan } = pl;
  const of = (k: Stage['kind']) => plan.stages.filter(s => s.kind === k);
  const groups = of('group'), seconds = of('repechage'), final = of('final')[0];
  const direct = groups.length && final ? final.seats.filter(z => z.kind === 'rank' && groups.some(g => g.id === z.stage)).length / groups.length : 0;
  const pname = (s?: Stage) => s ? presetName(pl.presetOf(s)) : '';
  switch (kind) {
    case 'field':
      if (!groups.length) return [t('tt.ex.field.one', { n: plan.drivers })];
      return [t('tt.ex.field.groups', { n: plan.drivers, g: groups.length, size: sizesText(groups.map(g => g.seats.length)) }),
        t(x.split === 'snake' ? 'tt.ex.field.snake' : 'tt.ex.field.random')];
    case 'group': {
      if (!groups.length) return [t('tt.ex.none.group')];
      const g0 = groups[0];
      const out = [t('tt.ex.group.race', { preset: pname(g0), k: pl.presetOf(g0).racesPerLane, races: pl.est.get(g0.id)!.races }),
        t('tt.ex.group.points')];
      out.push(seconds.length ? t('tt.ex.group.toSecond', { q: direct, next: ordinal(direct + 1) })
        : t('tt.ex.group.toFinal', { q: direct }));
      return out;
    }
    case 'repechage': {
      if (!seconds.length) return [t('tt.ex.none.second')];
      const w = (final.seats.length - direct * groups.length) / seconds.length;
      return [t(seconds.length === 1 ? 'tt.ex.second.who1' : 'tt.ex.second.whoN', { m: seconds.length, size: sizesText(seconds.map(s => s.seats.length)) }),
        t('tt.ex.second.mix'),
        t(w === 1 ? 'tt.ex.second.win1' : 'tt.ex.second.winN', { w, r: plan.repechageSeats })];
    }
    case 'final':
      if (!final) return [];
      return [t('tt.ex.final.race', { n: final.seats.length, preset: pname(final), races: pl.est.get(final.id)!.races }),
        t('tt.ex.final.result')];
    case 'smallFinal': {
      const k = of('smallFinal')[0];
      if (!k) return [t('tt.ex.smallFinal.none', { n: plan.smallFinalSize })];
      return [t('tt.ex.smallFinal'), t('tt.ex.smallFinal.race', { n: k.seats.length, preset: pname(k), races: pl.est.get(k.id)!.races })];
    }
    case 'rules':
      return [t('tt.ex.rules.1'), t('tt.ex.rules.2')];
  }
}

/** The tournament's race rules: the same for every race of the evening (GUI_SPEC §5.2a). */
function RulesFields({ x }: { x: TournamentTemplate }) {
  const r = x.rules;
  const pp = connection.value.powerPack;
  const setR = (patch: Partial<RaceRules>) => updateTemplate(x.id, { rules: { ...r, ...patch } });
  return html`
        <div class="tt-setting">
          <h3 class="tt-sub-h">${t('rp.sec.crashes')}</h3>
          <${Field} label=${t('rp.crash')}
              hint=${t(r.crashPolicy === 'crashIsDnf' ? 'rp.crash.out.hint' : 'rp.crash.replace.hint')}>
            <${Segmented} label=${t('rp.crash')} value=${r.crashPolicy}
              onChange=${(crashPolicy: RaceRules['crashPolicy']) => setR({ crashPolicy })}
              options=${[{ value: 'crashIsDnf', label: t('rp.crash.out') }, { value: 'replacementAllowed', label: t('rp.crash.replace') }]} />
          <//>
          ${r.crashPolicy === 'crashIsDnf' && html`<${Field} label=${t('rp.basis')}
              hint=${t(r.resultBasis === 'distance' ? 'rp.basis.distance.hint' : 'rp.basis.crashOrder.hint')}>
            <${Segmented} label=${t('rp.basis')} value=${r.resultBasis}
              onChange=${(resultBasis: RaceRules['resultBasis']) => setR({ resultBasis })}
              options=${[
                { value: 'crashOrder', label: t('rp.basis.crashOrder') },
                { value: 'distance', label: t('rp.basis.distance'), disabled: !pp,
                  badge: pp ? undefined : t('pp.badge'), title: pp ? undefined : t('rp.basis.locked') },
              ]} />
            ${r.resultBasis === 'distance' && !pp && html`<p class="field-warn" role="note">${t('rp.basis.fallback')}</p>`}
          <//>`}
        </div>
        <div class="tt-setting">
          <h3 class="tt-sub-h">${t('rp.sec.falseStart')}</h3>
          <div class="field-row">
            <${Field} label=${t('rp.fs')} hint=${t('rp.fs.hint')}>
              <${Segmented} label=${t('rp.fs')} value=${r.falseStart.action}
                onChange=${(action: RaceRules['falseStart']['action']) => setR({ falseStart: { ...r.falseStart, action } })}
                options=${[{ value: 'timePenalty', label: t('rp.fs.penalty') }, { value: 'flag', label: t('rp.fs.flag') }]} />
            <//>
            ${r.falseStart.action === 'timePenalty' && html`<${Field} label=${t('rp.fs.seconds')} id="tt-fs">
              <${Stepper} id="tt-fs" label=${t('rp.fs.seconds')} value=${r.falseStart.penaltyMs / 1000} unit="s"
                min=${RULE_LIMITS.penaltySeconds.min} max=${RULE_LIMITS.penaltySeconds.max} step=${0.5} decimals=${1}
                onChange=${(s: number) => setR({ falseStart: { ...r.falseStart, penaltyMs: Math.round(s * 1000) } })} />
            <//>`}
            ${r.falseStart.action === 'timePenalty' && html`<${Field} label=${t('rp.fs.apply')}>
              <${Segmented} label=${t('rp.fs.apply')} value=${r.falseStart.apply}
                onChange=${(apply: RaceRules['falseStart']['apply']) => setR({ falseStart: { ...r.falseStart, apply } })}
                options=${[{ value: 'addToFirstLap', label: t('rp.fs.apply.firstLap') }, { value: 'addToTotal', label: t('rp.fs.apply.total') }]} />
            <//>`}
          </div>
        </div>
  `;
}

function Inspector({ x, pl }: { x: TournamentTemplate; pl: Planned }) {
  const kinds: Tab[] = ['field', ...(['group', 'repechage', 'final'] as const).filter(k => pl.plan.stages.some(s => s.kind === k)),
    ...(pl.plan.smallFinalSize >= 2 ? ['smallFinal' as const] : []), 'rules'];
  const kind = kinds.includes(selectedKind.value) ? selectedKind.value : kinds.includes('final') ? 'final' : kinds[0];
  const set = (patch: Partial<TournamentTemplate>) => updateTemplate(x.id, patch);
  if (!pl.plan.stages.length) return null;

  let settings: unknown;
  if (kind === 'field' && !pl.plan.stages.some(s => s.kind === 'group')) settings = html`<p class="tt-note tt-none">${t('tt.noSettings.field')}</p>`;
  else if (kind === 'field') settings = html`<div class="tt-setting">
      <${Field} label=${t('tpl.split')} hint=${t(x.split === 'snake' ? 'tpl.split.snake.hint' : 'tpl.split.random.hint')}>
        <${Segmented} label=${t('tpl.split')} value=${x.split} onChange=${(split: TournamentTemplate['split']) => set({ split })}
          options=${[{ value: 'snake', label: t('tpl.split.snake') }, { value: 'random', label: t('tpl.split.random') }]} />
      <//>
    </div>`;
  else if (kind === 'group') settings = html`
    <div class="tt-setting"><${PresetPick} id="tt-pg" label=${t('tt.preset.group')} value=${x.groupPreset}
      onChange=${(groupPreset: string) => set({ groupPreset })} /></div>
    <${TieField} x=${x} />`;
  else if (kind === 'repechage') settings = html`
    <div class="tt-setting">
      <${PresetPick} id="tt-pr" label=${t('tt.preset.second')} value=${x.repechagePreset}
        onChange=${(repechagePreset: string) => set({ repechagePreset })} />
      <${Field} label=${t('tpl.repSeats')} id="tt-rs"
          hint=${t(x.repechageSeats === 'auto' ? 'tpl.repSeats.auto.hint' : 'tpl.repSeats.fixed.hint')}>
        <div class="field-with-action">
          <${Segmented} label=${t('tpl.repSeats')} value=${x.repechageSeats === 'auto' ? 'auto' : 'fixed'}
            onChange=${(v: 'auto' | 'fixed') => set({ repechageSeats: v === 'auto' ? 'auto' : pl.plan.repechageSeats || 2 })}
            options=${[{ value: 'auto', label: t('tpl.repSeats.auto') }, { value: 'fixed', label: t('tpl.repSeats.fixed') }]} />
          ${x.repechageSeats !== 'auto' && html`<${Stepper} id="tt-rs" label=${t('tpl.repSeats')} value=${x.repechageSeats}
            min=${LIMITS.repechageSeats.min} max=${LIMITS.repechageSeats.max}
            onChange=${(repechageSeats: number) => set({ repechageSeats })} />`}
        </div>
      <//>
    </div>
    <${TieField} x=${x} />`;
  else if (kind === 'rules') settings = html`<${RulesFields} x=${x} />`;
  else if (kind === 'smallFinal') settings = html`<div class="tt-setting">
      <${Field} label=${t('tt.smallFinal.race')} hint=${t(x.smallFinal ? 'tt.smallFinal.on' : 'tt.smallFinal.off')}>
        <${Segmented} label=${t('tt.smallFinal.race')} value=${x.smallFinal ? 'yes' : 'no'}
          onChange=${(v: 'yes' | 'no') => set({ smallFinal: v === 'yes' })}
          options=${[{ value: 'yes', label: t('gp.yes') }, { value: 'no', label: t('gp.no') }]} />
      <//>
    </div>`;
  else settings = html`<div class="tt-setting"><${PresetPick} id="tt-pf"
      label=${t(pl.plan.stages.length === 1 ? 'tt.preset.only' : 'tt.preset.final')} value=${x.finalPreset}
      onChange=${(finalPreset: string) => set({ finalPreset })} /></div>`;

  return html`<section class="tt-inspector" aria-labelledby="tt-insp-title">
    <nav class="tt-insp-tabs" aria-label=${t('tt.inspector')}>
      ${kinds.map(k => html`<button type="button" class=${'tt-insp-tab' + (k === kind ? ' on' : '')} aria-pressed=${k === kind}
          onClick=${() => { selectedKind.value = k; }}><span>${t(`tt.kind.${k}` as Key)}</span></button>`)}
    </nav>
    <div class="tt-insp-body">
      <div class="tt-explain">
        <h2 id="tt-insp-title">${t(`tt.insp.${kind}` as Key)}</h2>
        ${explain(kind, x, pl).map(s => html`<p>${s}</p>`)}
      </div>
      <div class="tt-settings">${settings}</div>
    </div>
  </section>`;
}

// --- page ------------------------------------------------------------------------------------

function Tabs({ list, selected }: { list: TournamentTemplate[]; selected: string }) {
  return html`<nav class="tt-tabs" aria-label=${t('tpl.listLabel')}>
    ${list.map(x => html`<button type="button" key=${x.id} class=${'tt-tab' + (x.id === selected ? ' on' : '')}
        aria-current=${x.id === selected || undefined} onClick=${() => { selectedTemplateId.value = x.id; }}>
      <span>${templateName(x)}</span></button>`)}
    <button type="button" class="tt-tab tt-tab-add" onClick=${() => createTemplate()}><span>+ ${t('tpl.new')}</span></button>
  </nav>`;
}

export function TournamentsPage() {
  const list = templates.value;
  const x = list.find(y => y.id === selectedTemplateId.value) ?? list[0];
  const pl = usePlanned(x);
  return html`
    <header class="tt-head">
      <h1>${t('area.tournaments')}</h1>
      <${Tabs} list=${list} selected=${x.id} />
    </header>
    ${!templatesPersistent.value && html`<p class="pe-note warn" role="note">${t('tpl.notPersistent')}</p>`}
    ${templatesLoaded.value && html`
      <${TreeFrame} key=${x.id} x=${x} list=${list} pl=${pl} />
      <${Inspector} x=${x} pl=${pl} />`}`;
}
