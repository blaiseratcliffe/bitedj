// sets.js under Node: the loader, the fallbacks and allows(), with the file
// read stubbed so each test decides when a read completes.
//
// Run from bitedj/:  node --test res/visuals/tests/sets.test.js
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { createSets } = require('../sets.js');

const KNOBS = { bars: 16, reactivity: 1, bounce: 2, swirl: 2, camMix: 1 };

function file(sets) {
  return JSON.stringify({ version: 1, nextId: 10, known: { sketches: [], patterns: [], clips: [] }, sets });
}

const RANDOM = { id: 1, name: 'Random DnB', type: 'random',
  sketches: ['tunnel', 'pattern-flow', 'video-kaleid'], patterns: ['arcs_1'], clips: ['a.mp4'], knobs: KNOBS };
const OTHER = { id: 3, name: 'Mono only', type: 'random',
  sketches: ['contours', 'ribbons', 'flow-lines'], patterns: [], clips: [], knobs: KNOBS };
const SEQUENCE = { id: 2, name: 'Opener', type: 'sequence',
  entries: [{ sketch: 'logo-kaleid', intro: true }, { sketch: 'tunnel' }], knobs: KNOBS };

// A read stub that holds each request until the test releases it, so the
// order of completion is under the test's control. `text` may be a string,
// null (missing file) or an Error.
function rig(text) {
  const reads = [];
  const logs = [];
  const changes = [];
  const sets = createSets({
    path: 'sets.json',
    read: (p, cb) => reads.push({ p, cb }),
    log: (...a) => logs.push(a.join(' '))
  });
  sets.onChange(info => changes.push(info));
  const deliver = (i = reads.length - 1, body = text) => {
    const r = reads[i];
    if (body instanceof Error) r.cb(body, null);
    else if (body === null) r.cb(new Error('missing or empty'), null);
    else r.cb(null, body);
  };
  return { sets, reads, logs, changes, deliver };
}

test('before any update everything is allowed', () => {
  const h = rig(file([RANDOM]));
  assert.deepEqual(h.sets.active(), { id: 0, name: 'Everything', type: 'random' });
  assert.equal(h.sets.allows('sketch', 'anything'), true);
  assert.equal(h.sets.isSequence(), false);
});

test('a Random set allows only what it ticks, per kind', () => {
  const h = rig(file([RANDOM]));
  h.sets.update({ set: 1, setRev: 0 });
  assert.equal(h.reads.length, 1);
  h.deliver();
  assert.deepEqual(h.sets.active(), { id: 1, name: 'Random DnB', type: 'random' });
  assert.equal(h.sets.allows('sketch', 'tunnel'), true);
  assert.equal(h.sets.allows('sketch', 'contours'), false);
  assert.equal(h.sets.allows('pattern', 'arcs_1'), true);
  assert.equal(h.sets.allows('pattern', 'backpack-grid'), false);
  assert.equal(h.sets.allows('clip', 'a.mp4'), true);
  assert.equal(h.sets.allows('clip', 'b.mp4'), false);
  assert.deepEqual(h.changes, [{ id: 1, previousId: 0, type: 'random', reason: 'switch' }]);
});

test('the same set and rev read nothing again', () => {
  const h = rig(file([RANDOM]));
  h.sets.update({ set: 1, setRev: 4 });
  h.deliver();
  h.sets.update({ set: 1, setRev: 4 });
  h.sets.update({ set: 1, setRev: 4 });
  assert.equal(h.reads.length, 1);
});

// The app seeds visuals_set_rev with its start time in epoch seconds, so the
// page, which stays up while the app restarts, sees a new rev after every
// restart and rereads a file that was edited while the app was down.
test('an app restart is a new rev, and rereads the same set', () => {
  const h = rig(file([RANDOM]));
  h.sets.update({ set: 1, setRev: 1790000000 });
  h.deliver();
  h.sets.update({ set: 1, setRev: 1790000000 });
  assert.equal(h.reads.length, 1);
  const edited = Object.assign({}, RANDOM, { sketches: ['contours', 'ribbons', 'flow-lines'] });
  h.sets.update({ set: 1, setRev: 1790000642 });
  assert.equal(h.reads.length, 2);
  h.deliver(1, file([edited]));
  assert.equal(h.sets.allows('sketch', 'contours'), true);
  assert.equal(h.changes[1].reason, 'edit');
});

test('a new rev on the same set rereads and reports an edit', () => {
  const h = rig(file([RANDOM]));
  h.sets.update({ set: 1, setRev: 0 });
  h.deliver();
  const edited = Object.assign({}, RANDOM, { sketches: ['contours', 'ribbons', 'tunnel'] });
  h.sets.update({ set: 1, setRev: 1 });
  h.deliver(1, file([edited]));
  assert.equal(h.sets.allows('sketch', 'contours'), true);
  assert.equal(h.sets.allows('sketch', 'pattern-flow'), false);
  assert.equal(h.changes[1].reason, 'edit');
  assert.equal(h.changes[1].previousId, 1);
});

test('switching sets reports a switch with the previous id', () => {
  const h = rig(file([RANDOM, OTHER]));
  h.sets.update({ set: 1, setRev: 0 });
  h.deliver();
  h.sets.update({ set: 3, setRev: 0 });
  h.deliver();
  assert.deepEqual(h.changes[1], { id: 3, previousId: 1, type: 'random', reason: 'switch' });
  assert.equal(h.sets.allows('sketch', 'tunnel'), false);
});

test('set 0 is Everything and needs no read', () => {
  const h = rig(file([RANDOM]));
  h.sets.update({ set: 1, setRev: 0 });
  h.deliver();
  h.sets.update({ set: 0, setRev: 0 });
  assert.equal(h.reads.length, 1);
  assert.deepEqual(h.sets.active(), { id: 0, name: 'Everything', type: 'random' });
  assert.equal(h.sets.allows('sketch', 'contours'), true);
  assert.deepEqual(h.changes[1], { id: 0, previousId: 1, type: 'random', reason: 'switch' });
});

test('the first update of set 0 still reports, so the director logs what is in force', () => {
  const h = rig(file([]));
  h.sets.update({ set: 0, setRev: 0 });
  assert.equal(h.reads.length, 0);
  assert.deepEqual(h.changes, [{ id: 0, previousId: 0, type: 'random', reason: 'switch' }]);
});

for (const [label, body] of [
  ['broken JSON', '{"version": 1, "sets": ['],
  ['version 2', JSON.stringify({ version: 2, sets: [RANDOM] })],
  ['no sets array', JSON.stringify({ version: 1 })],
  ['a missing file', null],
  ['a read error', new Error('not readable')]
]) {
  test('an unreadable file (' + label + ') plays everything and says so', () => {
    const h = rig(body);
    h.sets.update({ set: 1, setRev: 0 });
    h.deliver();
    assert.ok(h.logs.some(l => l.startsWith('visuals: sets unreadable')), h.logs.join('\n'));
    assert.equal(h.sets.allows('sketch', 'contours'), true);
    assert.equal(h.sets.active().id, 0);
    assert.equal(h.changes[0].reason, 'unreadable');
  });
}

test('an id the file lacks plays everything', () => {
  const h = rig(file([RANDOM]));
  h.sets.update({ set: 7, setRev: 0 });
  h.deliver();
  assert.ok(h.logs.includes('visuals: sets 7 is not in the sets file; playing everything'), h.logs.join('\n'));
  assert.equal(h.sets.active().id, 0);
  assert.equal(h.changes[0].reason, 'switch');
});

test('a sequence plays everything until Part B', () => {
  const h = rig(file([SEQUENCE]));
  h.sets.update({ set: 2, setRev: 0 });
  h.deliver();
  assert.ok(h.logs.includes('visuals: sets 2 is a sequence; sequences are not supported by this page, playing everything'), h.logs.join('\n'));
  assert.equal(h.sets.isSequence(), false);
  assert.equal(h.sets.active().id, 0);
  assert.equal(h.sets.allows('sketch', 'contours'), true);
});

test('a slow read overtaken by a newer update is dropped', () => {
  const h = rig(file([RANDOM, OTHER]));
  h.sets.update({ set: 1, setRev: 0 });
  h.sets.update({ set: 3, setRev: 0 });
  h.deliver(1);            // the newer read lands first
  h.deliver(0);            // then the stale one
  assert.equal(h.sets.active().id, 3);
  assert.equal(h.changes.length, 1);
});

test('list fields that are not arrays of strings are ignored, not fatal', () => {
  const odd = { id: 4, name: 'Odd', type: 'random', sketches: ['tunnel', 5, null], patterns: 'arcs_1', knobs: KNOBS };
  const h = rig(file([odd]));
  h.sets.update({ set: 4, setRev: 0 });
  h.deliver();
  assert.equal(h.sets.allows('sketch', 'tunnel'), true);
  assert.equal(h.sets.allows('pattern', 'arcs_1'), false);
  assert.equal(h.sets.allows('clip', 'a.mp4'), false);
});

test('no path: nothing is read and everything is allowed', () => {
  const reads = [];
  const sets = createSets({ path: null, read: (p, cb) => reads.push(p), log: () => {} });
  sets.update({ set: 1, setRev: 3 });
  assert.equal(reads.length, 0);
  assert.equal(sets.allows('sketch', 'x'), true);
});

test('a throwing listener does not stop the others', () => {
  const h = rig(file([RANDOM]));
  const seen = [];
  h.sets.onChange(() => { throw new Error('boom'); });
  h.sets.onChange(info => seen.push(info.id));
  h.sets.update({ set: 1, setRev: 0 });
  h.deliver();
  assert.deepEqual(seen, [1]);
});
