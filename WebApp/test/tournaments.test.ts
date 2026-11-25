/*
 * Tournament templates and plans (GUI_SPEC §5): the shipped templates, the
 * scaling rules of §5.4 (its table), snake dealing and crossover, the
 * plan's validation, normalization and storage.
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import {
  BUILTIN_TEMPLATES, BUILTIN_TEMPLATE_IDS, duplicateTemplate, isTemplateAtDefaults, newTemplate, normalizeTemplate,
  presetsUsed, type TournamentTemplate,
} from '../src/tournaments/template.ts';
import {
  autoRepechageSeats, checkPlan, planTotals, planTournament, snakeDeal, validRepechageSeats, type Source, type Stage,
} from '../src/tournaments/plan.ts';
import { BUILTIN_IDS } from '../src/presets/preset.ts';
import { MemoryLibraryStorage } from '../src/store/libraryStorage.ts';

const tpl = (id: string) => BUILTIN_TEMPLATES.find(t => t.id === id)!;
const grf = tpl(BUILTIN_TEMPLATE_IDS.groupsRepechageFinal);
const gf = tpl(BUILTIN_TEMPLATE_IDS.groupsFinal);
const single = tpl(BUILTIN_TEMPLATE_IDS.single);

const sizes = (plan: ReturnType<typeof planTournament>, kind: Stage['kind']) =>
  plan.stages.filter(s => s.kind === kind).map(s => s.seats.length);
const label = (plan: ReturnType<typeof planTournament>, x: Source): string => {
  if (x.kind === 'seed') return `#${x.seed}`;
  const s = plan.stages.find(st => st.id === x.stage)!;
  return (s.kind === 'group' ? 'AB'[s.index] : s.id) + x.rank;
};

describe('shipped templates', () => {
  test('reference evening first; groups on the default race, repechage on the short race', () => {
    assert.deepEqual(BUILTIN_TEMPLATES.map(t => t.format), ['groupsRepechageFinal', 'groupsFinal', 'single']);
    assert.equal(grf.groupPreset, BUILTIN_IDS.default);
    assert.equal(grf.repechagePreset, BUILTIN_IDS.short);
    assert.equal(grf.finalPreset, BUILTIN_IDS.default);
    assert.equal(grf.tiebreakerPreset, BUILTIN_IDS.tiebreaker);
    assert.equal(grf.tieBreak, 'winsThenBestLap');
  });
  test('presets used depend on the format and the tie-break', () => {
    assert.deepEqual(presetsUsed(single), [BUILTIN_IDS.default]);
    assert.deepEqual(presetsUsed(grf).sort(), [BUILTIN_IDS.default, BUILTIN_IDS.short].sort());
    assert.ok(presetsUsed({ ...gf, tieBreak: 'tiebreakerRace' }).includes(BUILTIN_IDS.tiebreaker));
  });
  test('an edited shipped template stays shipped with its format, and knows its defaults', () => {
    assert.equal(isTemplateAtDefaults(grf), true);
    const edited = normalizeTemplate({ ...grf, format: 'single', repechageSeats: 3 })!;
    assert.equal(edited.builtIn, true);
    assert.equal(edited.format, 'groupsRepechageFinal');
    assert.equal(edited.repechageSeats, 3);
    assert.equal(isTemplateAtDefaults(edited), false);
  });
});

describe('normalization', () => {
  test('rejects non-templates, fills and clamps the rest', () => {
    assert.equal(normalizeTemplate(null), null);
    assert.equal(normalizeTemplate({ name: 'x' }), null);
    const t = normalizeTemplate({ id: 'u1', format: 'bogus', repechageSeats: 99, tieBreak: 'coin' })!;
    assert.equal(t.format, 'groupsRepechageFinal');
    assert.equal(t.repechageSeats, 5);
    assert.equal(t.tieBreak, 'winsThenBestLap');
    assert.equal(t.builtIn, false);
    assert.equal(normalizeTemplate({ id: 'u2', repechageSeats: 'x' })!.repechageSeats, 'auto');
  });
  test('new and duplicated templates are the user\'s', () => {
    assert.equal(newTemplate('a', 'A').builtIn, false);
    const d = duplicateTemplate(single, 'b', 'B');
    assert.deepEqual([d.builtIn, d.format, d.id], [false, 'single', 'b']);
  });
});

describe('snake dealing and repechage seats', () => {
  test('snake: 1 2 2 1 1 2 …, sizes differ by at most one', () => {
    assert.deepEqual(snakeDeal([1, 2, 3, 4, 5, 6, 7, 8], 2), [[1, 4, 5, 8], [2, 3, 6, 7]]);
    assert.deepEqual(snakeDeal([1, 2, 3, 4, 5, 6, 7], 3).map(p => p.length), [3, 2, 2]);
  });
  test('valid and auto repechage seats', () => {
    assert.deepEqual(validRepechageSeats(6, 2), [2, 4]);
    assert.equal(autoRepechageSeats(6, 2), 2);
    assert.equal(autoRepechageSeats(4, 2), 2);
    assert.equal(autoRepechageSeats(6, 3), 3);
    assert.equal(autoRepechageSeats(6, 6), null);
  });
});

describe('Groups → Repechage → Final scales with the field (GUI_SPEC §5.4 table)', () => {
  test('12 drivers, 6 lanes: 2 × 6, 2 + 2 direct, 8 → 2 × 4 winner each, final 6', () => {
    const p = planTournament(grf, 12, 6);
    assert.deepEqual(sizes(p, 'group'), [6, 6]);
    assert.deepEqual(sizes(p, 'repechage'), [4, 4]);
    assert.deepEqual(sizes(p, 'final'), [6]);
    assert.equal(p.repechageSeats, 2);
    assert.deepEqual(p.problems, []);
  });
  test('the reference evening\'s crossover: A3 B4 A5 B6 / B3 A4 B5 A6', () => {
    const p = planTournament(grf, 12, 6);
    const reps = p.stages.filter(s => s.kind === 'repechage');
    assert.deepEqual(reps.map(r => r.seats.map(x => label(p, x))), [['A3', 'B4', 'A5', 'B6'], ['B3', 'A4', 'B5', 'A6']]);
    const final = p.stages.find(s => s.kind === 'final')!;
    assert.deepEqual(final.seats.map(x => label(p, x)), ['A1', 'B1', 'A2', 'B2', 'R11', 'R21']);
  });
  test('groups are snaked by seed: the strongest spread evenly', () => {
    const p = planTournament(grf, 12, 6);
    assert.deepEqual(p.stages[0].seats.map(x => label(p, x)), ['#1', '#4', '#5', '#8', '#9', '#12']);
  });
  test('10 drivers, 6 lanes: 2 × 5, 6 → 1 × 6 top 2', () => {
    const p = planTournament(grf, 10, 6);
    assert.deepEqual(sizes(p, 'group'), [5, 5]);
    assert.deepEqual(sizes(p, 'repechage'), [6]);
    assert.deepEqual(p.routes.filter(r => r.from === 'R1').map(r => r.ranks), [[1, 2]]);
  });
  test('6 drivers, 6 lanes: the group is the final', () => {
    const p = planTournament(grf, 6, 6);
    assert.equal(p.groupIsFinal, true);
    assert.deepEqual(p.stages.map(s => s.kind), ['final']);
  });
  test('8 drivers, 4 lanes: 2 × 4, 1 + 1 direct, 6 → 2 × 3 winner each', () => {
    const p = planTournament(grf, 8, 4);
    assert.deepEqual(sizes(p, 'group'), [4, 4]);
    assert.deepEqual(sizes(p, 'repechage'), [3, 3]);
    assert.deepEqual(sizes(p, 'final'), [4]);
  });
  test('a repechage seat count that does not split evenly falls back to auto with a warning', () => {
    const p = planTournament({ ...grf, repechageSeats: 3 }, 12, 6);
    assert.equal(p.repechageSeats, 2);
    assert.deepEqual(p.problems.map(x => x.code), ['repechageSeats']);
    assert.equal(planTournament({ ...grf, repechageSeats: 4 }, 12, 6).repechageSeats, 4);
  });
  test('too large a field for one repechage round is refused', () => {
    assert.equal(planTournament(grf, 24, 6).problems[0].code, 'repechageTooBig');
    assert.equal(planTournament(grf, 4, 2).problems[0].code, 'noRepechage');
  });
  test('every plan sends every driver to one seat only, and no stage outgrows the track', () => {
    for (const L of [2, 4, 6]) for (let N = 2; N <= 30; N++) for (const t of [grf, gf, single]) {
      const p = planTournament(t, N, L);
      assert.ok(!p.problems.some(x => x.code === 'routedTwice' || x.code === 'stageTooBig'), `${t.format} ${N}/${L}`);
      if (p.finalStage) {
        const first = p.stages.filter(s => s.column === 1).reduce((n, s) => n + s.seats.length, 0);
        assert.equal(first, N, `${t.format} ${N}/${L}: everyone starts`);
      }
    }
  });
});

describe('Groups → Final and Single race', () => {
  test('14 drivers on 6 lanes: 3 groups, 2 each to a 6-seat final', () => {
    const p = planTournament(gf, 14, 6);
    assert.deepEqual(sizes(p, 'group'), [5, 5, 4]);
    assert.deepEqual(sizes(p, 'final'), [6]);
  });
  test('7 drivers on 6 lanes: 4 + 3, three each', () => {
    const p = planTournament(gf, 7, 6);
    assert.deepEqual(sizes(p, 'final'), [6]);
  });
  test('single race: everyone in one stage, refused beyond the lane count', () => {
    assert.deepEqual(sizes(planTournament(single, 5, 6), 'final'), [5]);
    assert.equal(planTournament(single, 7, 6).problems[0].code, 'singleTooBig');
    assert.equal(planTournament(single, 1, 6).problems[0].code, 'tooFewDrivers');
  });
});

describe('plan checks and totals', () => {
  test('a rank sent twice, a stage too big, empty final seats', () => {
    const seat = (stage: string, rank: number): Source => ({ kind: 'rank', stage, rank });
    const stages: Stage[] = [
      { id: 'G1', kind: 'group', index: 0, column: 1, presetId: 'p', seats: Array.from({ length: 7 }, (_, i) => ({ kind: 'seed', seed: i + 1 })) },
      { id: 'F', kind: 'final', index: 0, column: 2, presetId: 'p', seats: [seat('G1', 1), seat('G1', 1)] },
    ];
    assert.deepEqual(checkPlan(stages, 6, 7).map(x => x.code), ['stageTooBig', 'routedTwice', 'finalEmptySeats']);
  });
  test('totals add up the stages', () => {
    const p = planTournament(grf, 12, 6);
    const tot = planTotals(p, s => ({ races: s.seats.length, ms: 1000 * s.seats.length }));
    assert.equal(tot.races, 6 + 6 + 4 + 4 + 6);
    assert.equal(tot.ms, 26_000);
  });
});

describe('storage', () => {
  test('templates round-trip through the library storage, normalized', async () => {
    const s = new MemoryLibraryStorage();
    const t: TournamentTemplate = { ...newTemplate('u1', 'Mine'), repechageSeats: 4 };
    await s.putTemplate(t);
    assert.deepEqual(await s.listTemplates(), [t]);
    await s.deleteTemplate('u1');
    assert.deepEqual(await s.listTemplates(), []);
  });
});

describe('small final and a smaller final (Grand Prix options)', () => {
  const codes = (p: ReturnType<typeof planTournament>, id: string) =>
    p.stages.find(s => s.id === id)!.seats.map(z => z.kind === 'rank' ? `${z.stage}:${z.rank}` : `#${z.seed}`);

  test('reference evening: the small final is places 2–4 of both second-chance races', () => {
    const p = planTournament(grf, 12, 6, { smallFinal: true });
    assert.equal(p.smallFinalSize, 6);
    assert.deepEqual(codes(p, 'K'), ['R1:2', 'R2:2', 'R1:3', 'R2:3', 'R1:4', 'R2:4']);
    assert.equal(p.stages.find(s => s.id === 'K')!.column, p.stages.find(s => s.id === 'F')!.column);
    assert.deepEqual(p.problems, []);
  });

  test('without the option there is no small final, but its size is known', () => {
    const p = planTournament(grf, 12, 6);
    assert.equal(p.stages.some(s => s.kind === 'smallFinal'), false);
    assert.equal(p.smallFinalSize, 6);
  });

  test('8 drivers: final 6 + small final 2; one seat moved down: final 5 + small final 3', () => {
    const six = planTournament(grf, 8, 6, { smallFinal: true });
    assert.deepEqual(codes(six, 'F'), ['G1:1', 'G2:1', 'G1:2', 'G2:2', 'R1:1', 'R1:2']);
    assert.deepEqual(codes(six, 'K'), ['R1:3', 'R1:4']);
    const five = planTournament(grf, 8, 6, { smallFinal: true, finalSeats: 5 });
    assert.deepEqual(codes(five, 'F'), ['G1:1', 'G2:1', 'G1:2', 'G2:2', 'R1:1']);
    assert.deepEqual(codes(five, 'K'), ['R1:2', 'R1:3', 'R1:4']);
    assert.deepEqual(five.problems, []);
  });

  test('a final of 5 for 12 drivers: the plan adapts (1 direct seat per group, 3 second-chance races)', () => {
    const p = planTournament(grf, 12, 6, { smallFinal: true, finalSeats: 5 });
    assert.deepEqual(p.problems, []);
    assert.equal(p.stages.find(s => s.id === 'F')!.seats.length, 5);
    assert.equal(p.stages.filter(s => s.kind === 'repechage').length, 3);
    assert.equal(p.stages.find(s => s.id === 'K')!.seats.length, 6);
  });

  test('groups → final: the small final takes the next places of the groups', () => {
    const p = planTournament(gf, 12, 6, { smallFinal: true });
    assert.deepEqual(codes(p, 'K'), ['G1:4', 'G2:4', 'G1:5', 'G2:5', 'G1:6', 'G2:6']);
  });

  test('no small final for one race, or when the group is the final, or with fewer than 2 left', () => {
    assert.equal(planTournament(single, 6, 6, { smallFinal: true }).smallFinalSize, 0);
    assert.equal(planTournament(grf, 5, 6, { smallFinal: true }).smallFinalSize, 0);
    const seven = planTournament(gf, 7, 6, { smallFinal: true });     // 4 + 3, top 3 each: 1 left
    assert.equal(seven.stages.some(s => s.kind === 'smallFinal'), seven.smallFinalSize >= 2);
  });

  test('a smaller final does not warn about empty seats', () => {
    const p = planTournament(gf, 12, 6, { finalSeats: 4 });
    assert.equal(p.stages.find(s => s.id === 'F')!.seats.length, 4);
    assert.equal(p.problems.some(x => x.code === 'finalEmptySeats'), false);
  });
});
