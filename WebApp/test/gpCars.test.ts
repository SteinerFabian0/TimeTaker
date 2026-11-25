import { test } from 'node:test';
import assert from 'node:assert/strict';
import { typedLabel, normalizeColor, normalizeCars, inkOn, needsEdge, normalizeCarPresets, sameCars, laneColorVars, CAR_LABEL_MAX, CAR_PALETTE } from '../src/grandPrix/cars.ts';

test('the text on a car: letters and digits, at most CAR_LABEL_MAX', () => {
  assert.equal(typedLabel('07'), '07');
  assert.equal(typedLabel('B M W!'), 'BMW');
  assert.equal(typedLabel('#123-45'), '1234');
  assert.equal(typedLabel('Zoë7'), 'Zoë7');
  assert.equal([...typedLabel('x'.repeat(20))].length, CAR_LABEL_MAX);
});

test('colours are kept as #rrggbb', () => {
  assert.equal(normalizeColor('#FF7A00'), '#ff7a00');
  assert.equal(normalizeColor('#f0a'), '#ff00aa');
  assert.equal(normalizeColor('red'), null);
  assert.equal(normalizeColor(12), null);
  assert.ok(CAR_PALETTE.every(c => normalizeColor(c) === c));
});

test('stored cars: only lanes 1–6, defaults dropped, junk cleaned', () => {
  const cars = normalizeCars({
    1: { type: 'closed', color: '#123456', label: '007' },
    2: { type: 'open', color: null, label: '' },
    3: { type: 'truck', color: 'blue', label: 'BMW M3' },
    7: { type: 'closed' },
  });
  assert.deepEqual(cars, {
    1: { type: 'closed', color: '#123456', label: '007' },
    3: { type: 'open', color: null, label: 'BMWM' },
  });
  assert.deepEqual(normalizeCars(null), {});
  assert.deepEqual(normalizeCars([1, 2]), {});
});

test('text on a lane colour contrasts with it', () => {
  assert.equal(inkOn('#f2f2f2'), '#111111');
  assert.equal(inkOn('#f5d02a'), '#111111');
  assert.equal(inkOn('#1b1b1d'), '#ffffff');
  assert.equal(inkOn('#2357d9'), '#ffffff');
});

test('very dark and very light cars get an edge colour on the page', () => {
  assert.equal(needsEdge('#1b1b1d'), true);
  assert.equal(needsEdge('#f2f2f2'), true);
  assert.equal(needsEdge('#d81e1e'), false);
  assert.equal(needsEdge('#f5d02a'), false);
});

test('car presets: junk skipped, cars normalized; same cars compares the lanes in use', () => {
  const list = normalizeCarPresets([
    { id: 'a', name: ' GT ', cars: { 1: { type: 'closed', color: '#FFF', label: '1' } } },
    { id: 'a', name: 'dup' }, { id: 'b', name: '' }, null, { name: 'no id' },
  ]);
  assert.deepEqual(list, [{ id: 'a', name: 'GT', cars: { 1: { type: 'closed', color: '#ffffff', label: '1' } } }]);
  assert.deepEqual(normalizeCarPresets('x'), []);
  const a = { 1: { type: 'open' as const, color: null, label: '7' } };
  const b = { 1: { type: 'open' as const, color: null, label: '7' }, 5: { type: 'closed' as const, color: null, label: '' } };
  assert.equal(sameCars(a, b), false);
  assert.equal(sameCars(a, b, 4), true);
});

test('lane colour variables only for lanes with a colour', () => {
  assert.equal(laneColorVars({}), '');
  const css = laneColorVars({ 2: { type: 'open', color: '#1b1b1d', label: '' } });
  assert.match(css, /--lane-2: #1b1b1d; --lane-2-ink: #ffffff; --lane-2-fg: color-mix/);
  assert.doesNotMatch(css, /--lane-1/);
});
