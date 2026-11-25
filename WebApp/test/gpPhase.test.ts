import { test } from 'node:test';
import assert from 'node:assert/strict';
import { applyPhase, changedFields, choosePreset, phasePresetId, phasesOf, setPhaseField } from '../src/grandPrix/phase.ts';
import { laneLightMasks, laneMask } from '../src/track/laneLights.ts';
import { BUILTIN_PRESETS, BUILTIN_IDS } from '../src/presets/preset.ts';
import { planTournament } from '../src/tournaments/plan.ts';
import { BUILTIN_TEMPLATES, BUILTIN_TEMPLATE_IDS } from '../src/tournaments/template.ts';

const group = BUILTIN_PRESETS.find(p => p.id === BUILTIN_IDS.default)!;
const grf = BUILTIN_TEMPLATES.find(t => t.id === BUILTIN_TEMPLATE_IDS.groupsRepechageFinal)!;

test('phases: groups, second chance, finals (final and small final together)', () => {
  const ph = phasesOf(planTournament(grf, 12, 6, { smallFinal: true }));
  assert.deepEqual(ph.map(p => `${p.column}:${p.kind}:${p.stages.map(s => s.id).join('+')}`), ['1:groups:G1+G2', '2:second:R1+R2', '3:finals:F+K']);
});

test('the evening changes laps, time and races per lane on top of the preset', () => {
  assert.equal(applyPhase(group, undefined), group);
  const e = applyPhase(group, { laps: 3, racesPerLane: 2 });
  assert.equal(e.laps, 3);
  assert.equal(e.racesPerLane, 2);
  assert.equal(e.points, group.points);
  assert.equal(applyPhase(group, { laps: 500 }).laps, 99);                       // clamped
  assert.equal(applyPhase(group, { durationMs: 4 * 60_000 }).durationMs, 4 * 60_000);
  assert.deepEqual([...changedFields(group, { laps: 3 })], ['laps']);
});

test('a value back at the preset\'s is no change', () => {
  let ph = setPhaseField(undefined, group, 'laps', 3);
  assert.deepEqual(ph, { laps: 3 });
  ph = setPhaseField(ph, group, 'laps', group.laps);
  assert.deepEqual(ph, {});
  assert.deepEqual(setPhaseField({}, group, 'sections', 2), {});                 // 2 is the default
  assert.deepEqual(setPhaseField({}, group, 'sections', 3), { sections: 3 });
});

test('another preset for a phase: drops the old preset\'s changes, keeps the sections', () => {
  const id = BUILTIN_IDS.default;
  assert.equal(phasePresetId(undefined, id), id);
  const ph = choosePreset({ laps: 3, sections: 3 }, id, BUILTIN_IDS.qualifying);
  assert.deepEqual(ph, { sections: 3, presetId: BUILTIN_IDS.qualifying });
  assert.equal(phasePresetId(ph, id), BUILTIN_IDS.qualifying);
  assert.deepEqual(choosePreset(ph, id, id), { sections: 3 });                 // back to the tournament's
});

test('lane lights: on for the used lanes, off for the rest', () => {
  assert.equal(laneMask([1, 3]), 0b101);
  assert.deepEqual(laneLightMasks([1, 2, 3, 4, 5], 6), { on: 0b011111, off: 0b100000 });
  assert.deepEqual(laneLightMasks([1, 2, 4, 5, 6], 6), { on: 0b111011, off: 0b000100 });
  assert.deepEqual(laneLightMasks([], 4), { on: 0, off: 0b1111 });
});
