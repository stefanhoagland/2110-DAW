import test from 'node:test';
import assert from 'node:assert/strict';
import { namesFromCsv, namesToCsv, parseCsv } from '../js/csv.js';

test('round trip keeps names, including commas and quotes', () => {
  const names = ['Kick', 'Snare, top', 'Vox "lead"', 'Ch 4'];
  assert.deepEqual(namesFromCsv(namesToCsv(names)).map((x) => x.name), names);
});

test('header in any order', () => {
  assert.deepEqual(namesFromCsv('Name,Channel\nBass,3\nKeys L,5\n'), [
    { channel: 3, name: 'Bass' }, { channel: 5, name: 'Keys L' },
  ]);
});

test('no header, numbered rows', () => {
  assert.deepEqual(namesFromCsv('1,Kick\r\n2,Snare\r\n'), [{ channel: 1, name: 'Kick' }, { channel: 2, name: 'Snare' }]);
});

test('plain list of names', () => {
  assert.deepEqual(namesFromCsv('Kick\nSnare\n\nHat\n').map((x) => x.channel), [1, 2, 3]);
});

test('Excel-style semicolons and BOM', () => {
  assert.deepEqual(namesFromCsv('﻿Ch;Name\n1;Kick\n'), [{ channel: 1, name: 'Kick' }]);
});

test('parseCsv handles quoted newlines', () => {
  assert.deepEqual(parseCsv('a,"b\nc"\n'), [['a', 'b\nc']]);
});
