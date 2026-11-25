import { test } from 'node:test';
import assert from 'node:assert/strict';
import { runOrder, splitRaces } from '../src/tournaments/runOrder.ts';
import { planTournament } from '../src/tournaments/plan.ts';
import { BUILTIN_TEMPLATES, BUILTIN_TEMPLATE_IDS } from '../src/tournaments/template.ts';

const grf = BUILTIN_TEMPLATES.find(t => t.id === BUILTIN_TEMPLATE_IDS.groupsRepechageFinal)!;
// The reference presets: groups and final 3×/lane, second chance 2×/lane, block rotation.
const k = (kind: string) => kind === 'repechage' ? 2 : 3;
const races = (s: { kind: string; seats: readonly unknown[] }) => k(s.kind) * s.seats.length;
const unit = (s: { kind: string }) => k(s.kind);
const show = (o: ReturnType<typeof runOrder>) => o.map(x => `${x.stage}${x.parts > 1 ? `.${x.part + 1}` : ''}:${x.races}`).join(' ');

test('split on lane changes, as even as possible', () => {
  assert.deepEqual(splitRaces(18, 3, 2), [9, 9]);
  assert.deepEqual(splitRaces(18, 3, 3), [6, 6, 6]);
  assert.deepEqual(splitRaces(15, 3, 2), [9, 6]);        // 5 lane changes: 3 + 2
  assert.deepEqual(splitRaces(18, 1, 4), [5, 5, 4, 4]);
  assert.deepEqual(splitRaces(6, 3, 3), [3, 3]);          // never more parts than lane changes
  assert.deepEqual(splitRaces(0, 3, 2), []);
});

test('reference evening, 2 sections: groups and second chances take turns, the final in one go', () => {
  const plan = planTournament(grf, 12, 6);
  assert.equal(show(runOrder(plan, 2, races, unit)), 'G1.1:9 G2.1:9 G1.2:9 G2.2:9 R1.1:4 R2.1:4 R1.2:4 R2.2:4 F:18');
  assert.equal(show(runOrder(plan, 1, races, unit)), 'G1:18 G2:18 R1:8 R2:8 F:18');
});

test('with a small final: it alternates with the final, which ends the evening', () => {
  const plan = planTournament(grf, 12, 6, { smallFinal: true });
  const o = runOrder(plan, 2, races, unit);
  assert.equal(show(o.slice(-4)), 'K.1:9 F.1:9 K.2:9 F.2:9');
  assert.equal(o.reduce((a, x) => a + x.races, 0), 18 + 18 + 8 + 8 + 18 + 18);
  assert.deepEqual(o.filter(x => x.stage === 'G1').map(x => x.start), [0, 9]);
});
