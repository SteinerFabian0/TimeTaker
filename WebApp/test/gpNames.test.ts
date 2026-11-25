import { test } from 'node:test';
import assert from 'node:assert/strict';
import { typedName, cleanName, keepName, shortCodes, NAME_MAX } from '../src/grandPrix/names.ts';

test('only letters and single spaces stay', () => {
  assert.equal(typedName('Anna!'), 'Anna');
  assert.equal(typedName('R2-D2'), 'RD');
  assert.equal(typedName('  Jürgen   M.'), 'Jürgen M');
  assert.equal(typedName('Zoë 🏎'), 'Zoë ');
  assert.equal(cleanName('Zoë 🏎'), 'Zoë');
  assert.equal(typedName('x'.repeat(40)).length, NAME_MAX);
});

test('a taken name gets the next free number', () => {
  assert.equal(keepName('Anna', ['Ben']), 'Anna');
  assert.equal(keepName('anna', ['Anna']), 'anna 2');
  assert.equal(keepName('Anna', ['Anna', 'Anna 2']), 'Anna 3');
  assert.equal(keepName('Anna B', ['Anna', 'Anna 2']), 'Anna B');
  assert.equal(keepName(' !! ', ['Anna']), '');
  assert.equal(keepName('Maximilianus', ['Maximilianus']), 'Maximilian 2', 'the number fits within NAME_MAX');
});

test('shortCodes: three letters, unique, F1 style', () => {
  assert.deepEqual(shortCodes(['Anna Berger', 'Max', 'Jo']), ['ANN', 'MAX', 'JO']);
  assert.deepEqual(shortCodes(['Anna', 'Annabelle']), ['ANA', 'ANE']);
  assert.deepEqual(shortCodes(['Max Huber', 'Max Weber']), ['MHU', 'MWE']);
  assert.deepEqual(shortCodes(['Anna', 'Anna 2']), ['ANA', 'AN2']);
  const many = shortCodes(['Ann', 'Ann', 'Ann']);
  assert.equal(new Set(many).size, 3);
  assert.ok(many.every(c => c.length <= 3));
});
