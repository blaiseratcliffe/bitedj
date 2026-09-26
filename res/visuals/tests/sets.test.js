// sets.js under Node: the loader, the fallbacks, allows(), and the sequence
// walker (next, peek, replay, restart), with the file read stubbed so each
// test decides when a read completes.
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

// Part B: the sequence walker.

const ENTRIES = (list) => list.map((e) => (typeof e === 'string' ? { sketch: e } : e));
function sequence(id, entries, name = 'Seq') {
  return { id, name, type: 'sequence', entries: ENTRIES(entries), knobs: KNOBS };
}
const all = () => true;
const names = (sets, n, canPlay = all) => {
  const out = [];
  for (let i = 0; i < n; i++) {
    const e = sets.next(canPlay);
    out.push(e ? e.sketch : null);
  }
  return out;
};
function loadSeq(entries, extra = []) {
  const h = rig(file([sequence(2, entries)].concat(extra)));
  h.sets.update({ set: 2, setRev: 0 });
  h.deliver();
  return h;
}

test('a sequence is a sequence, and its entries are what it allows', () => {
  const h = loadSeq(['tunnel', { sketch: 'pattern-flow', pattern: 'arcs_1' }, { sketch: 'video-kaleid', clip: 'a.mp4' }]);
  assert.equal(h.sets.isSequence(), true);
  assert.deepEqual(h.sets.active(), { id: 2, name: 'Seq', type: 'sequence' });
  assert.equal(h.sets.allows('sketch', 'tunnel'), true);
  assert.equal(h.sets.allows('sketch', 'contours'), false);
  assert.equal(h.sets.allows('pattern', 'arcs_1'), true);
  assert.equal(h.sets.allows('pattern', 'backpack-grid'), false);
  assert.equal(h.sets.allows('clip', 'a.mp4'), true);
  assert.equal(h.sets.allows('clip', 'b.mp4'), false);
  assert.ok(!h.logs.some(l => l.includes('not supported')));
});

test('entries come back in order, shaped as the contract says', () => {
  const h = loadSeq([{ sketch: 'logo-kaleid', intro: true, bars: 32 }, 'tunnel', { sketch: 'video-kaleid', clip: 'a.mp4', bars: 8 }]);
  assert.deepEqual(h.sets.next(all), { sketch: 'logo-kaleid', pattern: null, clip: null, bars: 32, intro: true, index: 1, total: 3 });
  assert.deepEqual(h.sets.next(all), { sketch: 'tunnel', pattern: null, clip: null, bars: null, intro: false, index: 2, total: 3 });
  assert.deepEqual(h.sets.next(all), { sketch: 'video-kaleid', pattern: null, clip: 'a.mp4', bars: 8, intro: false, index: 3, total: 3 });
});

test('intro once, then the rest loops', () => {
  const h = loadSeq([{ sketch: 'intro', intro: true }, 'v1', 'v2']);
  assert.deepEqual(names(h.sets, 7), ['intro', 'v1', 'v2', 'v1', 'v2', 'v1', 'v2']);
});

test('several intros play in order, once', () => {
  const h = loadSeq([{ sketch: 'i1', intro: true }, { sketch: 'i2', intro: true }, 'v1']);
  assert.deepEqual(names(h.sets, 4), ['i1', 'i2', 'v1', 'v1']);
});

test('repeats are allowed: intro, vis1, vis2, vis1', () => {
  const h = loadSeq([{ sketch: 'intro', intro: true }, 'vis1', 'vis2', 'vis1']);
  assert.deepEqual(names(h.sets, 8), ['intro', 'vis1', 'vis2', 'vis1', 'vis1', 'vis2', 'vis1', 'vis1']);
});

test('an unplayable loop entry is skipped this time round and tried again next time', () => {
  const h = loadSeq(['a', 'b', 'c']);
  let bOk = false;
  const canPlay = (e) => e.sketch !== 'b' || bOk;
  assert.deepEqual(names(h.sets, 3, canPlay), ['a', 'c', 'a']);
  bOk = true;
  assert.deepEqual(names(h.sets, 3, canPlay), ['b', 'c', 'a']);
});

test('an unplayable intro is skipped for good', () => {
  const h = loadSeq([{ sketch: 'cam-edges', intro: true }, 'v1', 'v2']);
  let camOk = false;
  const canPlay = (e) => e.sketch !== 'cam-edges' || camOk;
  assert.deepEqual(names(h.sets, 2, canPlay), ['v1', 'v2']);
  camOk = true;
  assert.deepEqual(names(h.sets, 3, canPlay), ['v1', 'v2', 'v1']);
});

test('nothing in the loop can play: null, and the position stays at the loop start', () => {
  const h = loadSeq([{ sketch: 'i', intro: true }, 'a', 'b']);
  let ok = false;
  const canPlay = (e) => e.intro || ok;
  assert.equal(h.sets.next(canPlay).sketch, 'i');
  assert.equal(h.sets.next(canPlay), null);
  assert.equal(h.sets.next(canPlay), null);
  ok = true;
  assert.equal(h.sets.next(canPlay).sketch, 'a');
});

test('an all-unplayable sequence returns null, intros included', () => {
  const h = loadSeq([{ sketch: 'i', intro: true }, 'a']);
  assert.equal(h.sets.next(() => false), null);
  assert.equal(h.sets.peek(() => false), null);
});

test('peek does not move', () => {
  const h = loadSeq([{ sketch: 'i', intro: true }, 'a', 'b']);
  assert.equal(h.sets.peek(all).sketch, 'i');
  assert.equal(h.sets.peek(all).sketch, 'i');
  assert.equal(h.sets.next(all).sketch, 'i');
  assert.equal(h.sets.peek(all).sketch, 'a');
});

test('replay gives the interrupted entry again, if it can still play', () => {
  const h = loadSeq(['a', 'b', 'c']);
  h.sets.next(all);
  assert.equal(h.sets.next(all).sketch, 'b');
  assert.equal(h.sets.replay(all).sketch, 'b');
  assert.equal(h.sets.next(all).sketch, 'c');
  assert.equal(h.sets.replay((e) => e.sketch !== 'c').sketch, 'a');
});

test('replay before any next is next', () => {
  const h = loadSeq([{ sketch: 'i', intro: true }, 'a']);
  assert.equal(h.sets.replay(all).sketch, 'i');
  assert.equal(h.sets.next(all).sketch, 'a');
});

test('restart plays the intro again', () => {
  const h = loadSeq([{ sketch: 'i', intro: true }, 'a', 'b']);
  names(h.sets, 4);
  h.sets.restart();
  assert.deepEqual(names(h.sets, 3), ['i', 'a', 'b']);
});

test('switching into a sequence starts it from entry 1', () => {
  const h = rig(file([sequence(2, [{ sketch: 'i', intro: true }, 'a', 'b']), RANDOM]));
  h.sets.update({ set: 2, setRev: 0 });
  h.deliver();
  names(h.sets, 3);
  h.sets.update({ set: 1, setRev: 0 });
  h.deliver();
  assert.equal(h.sets.isSequence(), false);
  h.sets.update({ set: 2, setRev: 0 });
  h.deliver();
  assert.equal(h.sets.next(all).sketch, 'i');
});

test('an edit of the playing sequence keeps its place and does not replay the intro', () => {
  const h = loadSeq([{ sketch: 'i', intro: true }, 'a', 'b', 'c']);
  assert.deepEqual(names(h.sets, 2), ['i', 'a']);
  h.sets.update({ set: 2, setRev: 1 });
  h.deliver(1, file([sequence(2, [{ sketch: 'i', intro: true }, 'a', 'b', 'c', 'd'])]));
  assert.deepEqual(names(h.sets, 4), ['b', 'c', 'd', 'a']);
});

test('an edit that adds intro entries mid-set does not play them', () => {
  const h = loadSeq([{ sketch: 'i', intro: true }, 'a', 'b']);
  assert.deepEqual(names(h.sets, 2), ['i', 'a']);   // the next would be b, at index 2
  h.sets.update({ set: 2, setRev: 1 });
  h.deliver(1, file([sequence(2, [
    { sketch: 'i1', intro: true }, { sketch: 'i2', intro: true }, { sketch: 'i3', intro: true }, 'a', 'b'])]));
  assert.deepEqual(names(h.sets, 3), ['a', 'b', 'a']);
});

test('intros passed while nothing could play are not played later', () => {
  const h = loadSeq([{ sketch: 'i', intro: true }, 'a']);
  assert.equal(h.sets.next(() => false), null);
  assert.equal(h.sets.next(all).sketch, 'a');
});

test('an edit that shortens the sequence under the position wraps to the loop', () => {
  const h = loadSeq([{ sketch: 'i', intro: true }, 'a', 'b', 'c', 'd']);
  names(h.sets, 4);                          // i a b c; the next would be d
  h.sets.update({ set: 2, setRev: 1 });
  h.deliver(1, file([sequence(2, [{ sketch: 'i', intro: true }, 'a', 'b'])]));
  assert.equal(h.sets.next(all).sketch, 'a');
});

test('a sequence file that turns unreadable plays everything', () => {
  const h = loadSeq(['a', 'b']);
  h.sets.update({ set: 2, setRev: 1 });
  h.deliver(1, 'not json');
  assert.equal(h.sets.isSequence(), false);
  assert.equal(h.sets.allows('sketch', 'anything'), true);
  assert.equal(h.sets.next(all), null);
});

test('a canPlay that throws counts the entry as unplayable', () => {
  const h = loadSeq(['a', 'b']);
  const canPlay = (e) => { if (e.sketch === 'a') throw new Error('boom'); return true; };
  assert.equal(h.sets.next(canPlay).sketch, 'b');
  assert.ok(h.logs.some(l => l.includes('boom')));
});

test('an intro after a loop entry is treated as a loop entry', () => {
  const h = loadSeq(['a', { sketch: 'late', intro: true }, 'b']);
  assert.deepEqual(names(h.sets, 4), ['a', 'late', 'b', 'a']);
});

// Fix round 1.

test('replay after an edit that adds intros does not play them', () => {
  const h = loadSeq([{ sketch: 'i', intro: true }, 'a', 'b']);
  assert.deepEqual(names(h.sets, 2), ['i', 'a']);   // interrupted during a
  h.sets.update({ set: 2, setRev: 1 });
  h.deliver(1, file([sequence(2, [
    { sketch: 'i1', intro: true }, { sketch: 'i2', intro: true }, { sketch: 'i3', intro: true }, 'a', 'b'])]));
  const e = h.sets.replay(all);
  assert.equal(e.intro, false);
  assert.equal(e.sketch, 'a');
});

test('replay after an edit that moves the interrupted entry does not play a different one', () => {
  const h = loadSeq(['a', 'b', 'c']);
  assert.deepEqual(names(h.sets, 2), ['a', 'b']);   // interrupted during b
  h.sets.update({ set: 2, setRev: 1 });
  h.deliver(1, file([sequence(2, ['x', 'a', 'b', 'c'])]));
  assert.equal(h.sets.replay(all).sketch, 'b');
});

test('replay after an edit that leaves the interrupted entry in place resumes it', () => {
  const h = loadSeq(['a', 'b', 'c']);
  assert.deepEqual(names(h.sets, 2), ['a', 'b']);
  h.sets.update({ set: 2, setRev: 1 });
  h.deliver(1, file([sequence(2, ['a', 'b', 'c', 'd'])]));
  assert.equal(h.sets.replay(all).sketch, 'b');
  assert.equal(h.sets.next(all).sketch, 'c');
});

test('replay after an edit never resumes an intro the walk has finished', () => {
  const h = loadSeq([{ sketch: 'i', intro: true }, 'a']);
  assert.equal(h.sets.next(all).sketch, 'i');       // the intros are done
  h.sets.update({ set: 2, setRev: 1 });
  h.deliver(1, file([sequence(2, [{ sketch: 'i', intro: true }, 'a', 'b'])]));
  assert.equal(h.sets.replay(all).sketch, 'a');
});

test('a canPlay that throws is logged once per call, however many entries throw', () => {
  const h = loadSeq(['a', 'b', 'c', 'd', 'e']);
  const boom = () => { throw new Error('boom'); };
  const count = () => h.logs.filter(l => l.includes('check failed')).length;
  assert.equal(h.sets.next(boom), null);
  assert.equal(count(), 1);
  assert.equal(h.sets.peek(boom), null);
  assert.equal(count(), 2);
  h.sets.next(all);
  assert.equal(h.sets.replay(boom), null);           // its own check and its fallback to next()
  assert.equal(count(), 3);
});

test('replay works detached from the sets object', () => {
  const h = loadSeq(['a', 'b']);
  const { next, replay } = h.sets;
  assert.equal(next(all).sketch, 'a');
  assert.equal(replay(all).sketch, 'a');
  // a cannot play now, so replay falls back to next().
  assert.equal(replay((e) => e.sketch !== 'a').sketch, 'b');
});

for (const [label, entries] of [
  ['no entries', []],
  ['no entry with a sketch', [{ pattern: 'arcs_1' }, { sketch: 5 }]]
]) {
  test('a sequence with ' + label + ' plays everything', () => {
    const h = rig(file([{ id: 2, name: 'Empty', type: 'sequence', entries, knobs: KNOBS }]));
    h.sets.update({ set: 2, setRev: 0 });
    h.deliver();
    assert.ok(h.logs.includes('visuals: sets 2 is a sequence with no entries; playing everything'), h.logs.join('\n'));
    assert.equal(h.sets.isSequence(), false);
    assert.equal(h.sets.active().id, 0);
    assert.equal(h.sets.allows('sketch', 'contours'), true);
    assert.equal(h.sets.next(all), null);
  });
}
