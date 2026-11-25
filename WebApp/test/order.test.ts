/*
 * The host's own list order (race preset list, drag and drop).
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { applyOrder, dropIndex, insertAfter, moveId } from '../src/presets/order.ts';

const items = (...ids: string[]) => ids.map(id => ({ id }));
const ids = (xs: { id: string }[]) => xs.map(x => x.id);

describe('list order', () => {
  test('applyOrder: stored order first, unknown items after in their given order, stale ids ignored', () => {
    assert.deepEqual(ids(applyOrder(items('a', 'b', 'c', 'd'), ['c', 'gone', 'a'])), ['c', 'a', 'b', 'd']);
    assert.deepEqual(ids(applyOrder(items('a', 'b'), [])), ['a', 'b']);
  });
  test('moveId moves down and up, clamped', () => {
    assert.deepEqual(moveId(['a', 'b', 'c', 'd'], 0, 2), ['b', 'c', 'a', 'd']);
    assert.deepEqual(moveId(['a', 'b', 'c', 'd'], 3, 0), ['d', 'a', 'b', 'c']);
    assert.deepEqual(moveId(['a', 'b'], 0, 99), ['b', 'a']);
    assert.deepEqual(moveId(['a', 'b'], 5, 0), ['a', 'b']);
  });
  test('insertAfter: right after its source, at the end without one', () => {
    assert.deepEqual(insertAfter(['a', 'b', 'c'], 'x', 'a'), ['a', 'x', 'b', 'c']);
    assert.deepEqual(insertAfter(['a', 'b'], 'x', null), ['a', 'b', 'x']);
    assert.deepEqual(insertAfter(['a', 'b'], 'x', 'missing'), ['a', 'b', 'x']);
  });
  test('dropIndex: counts the other items whose centre is above the dragged centre', () => {
    const centres = [30, 90, 150, 210];
    assert.equal(dropIndex(centres, 0, 30), 0);     // not moved
    assert.equal(dropIndex(centres, 0, 100), 1);    // past b
    assert.equal(dropIndex(centres, 0, 999), 3);    // to the end
    assert.equal(dropIndex(centres, 3, 10), 0);     // to the top
    assert.equal(dropIndex(centres, 2, 80), 1);     // between a and b
  });
});
