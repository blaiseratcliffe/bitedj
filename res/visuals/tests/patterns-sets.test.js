// patterns.js with a visuals set in force: the prewarm loads only what the
// set allows, take() hands out only that, setChanged() evicts the rest, and
// a set change with nothing to evict does not pull the first load forward.
//
// Run from bitedj/:  node --test res/visuals/tests/patterns-sets.test.js
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { loadPage, setsFile, patternFixture } = require('./page-rig.js');

const KNOBS = { bars: 16, reactivity: 1, bounce: 2, swirl: 2, camMix: 1 };
const SLUGS = ['p1', 'p2', 'p3', 'p4', 'p5'];
const randomSet = (id, patterns) => ({ id, name: 'Set ' + id, type: 'random', sketches: [], patterns, clips: [], knobs: KNOBS });

function page(sets) {
  const fx = patternFixture(SLUGS);
  return loadPage({
    scripts: ['sets.js', 'patterns.js'],
    patternIndex: fx.index,
    xhr: fx.xhr,
    files: { 'sets.json': setsFile(sets) }
  });
}

// Every slug a prewarm finished loading, in order.
const loaded = (p) => p.logs.filter(l => l.startsWith('visuals: pattern ready ')).map(l => l.split(' ')[3]);
const cached = (p) => p.ctx.patterns.library().filter(x => x.cached).map(x => x.slug).sort();

// What the director's change handler does, by hand.
function switchTo(p, id) {
  p.ctx.sets.update({ set: id, setRev: 1 });
  p.run(100);
  return p.ctx.patterns.setChanged();
}

test('the prewarm loads only the patterns the set allows', () => {
  const p = page([randomSet(1, ['p2', 'p4'])]);
  switchTo(p, 1);
  p.run(120000);
  assert.deepEqual(loaded(p).slice().sort(), ['p2', 'p4']);
});

test('with Everything the prewarm fills the cache from the whole library', () => {
  const p = page([]);
  switchTo(p, 0);
  p.run(90000);
  assert.equal(cached(p).length, 3);
});

test('setChanged() evicts what the new set leaves out, and take() follows', () => {
  const p = page([]);
  switchTo(p, 0);
  p.run(90000);
  const before = cached(p);
  assert.equal(before.length, 3);
  const outside = SLUGS.find(s => !before.includes(s));
  p.files['sets.json'] = setsFile([randomSet(7, [outside])]);
  const gone = switchTo(p, 7);
  assert.equal(gone, 3);
  before.forEach(s => assert.ok(p.logs.includes('visuals: pattern evicted ' + s + ' not in the visuals set'), p.logs.join('\n')));
  assert.equal(p.ctx.patterns.ready(), false);
  assert.equal(p.ctx.patterns.take([]), null);
  p.run(30000);
  assert.equal(p.ctx.patterns.ready(), true);
  assert.equal(p.ctx.patterns.take([]).meta.slug, outside);
});

// A load in flight is not in the cache yet, so setChanged() cannot reach it.
// It lands after the set has moved on: usable() has to refuse it, and the
// next prewarm's sweepRetired() has to evict it, naming the set as the reason.
test('a pattern still loading when the set leaves it out is never handed out, and goes', () => {
  const p = page([]);
  switchTo(p, 0);
  p.run(10000);                              // the boot prewarm has started its first load
  const inFlight = p.xhrs.filter(u => u.startsWith('assets/patterns/')).map(u => /patterns\/([^/.]+)/.exec(u)[1]);
  assert.ok(inFlight.length > 0, 'no load started');
  assert.deepEqual(loaded(p), [], 'the first load finished before the set change');
  const loading = inFlight[0];
  const other = SLUGS.find(s => s !== loading);
  p.files['sets.json'] = setsFile([randomSet(7, [other])]);
  assert.equal(switchTo(p, 7), 0);
  p.run(3000);
  assert.deepEqual(loaded(p), [loading], 'the load in flight did not finish');
  assert.equal(p.ctx.patterns.ready(), false);
  assert.equal(p.ctx.patterns.take([]), null);
  p.run(25000);                              // past the next 20 s prewarm tick
  assert.ok(p.logs.includes('visuals: pattern evicted ' + loading + ' not in the visuals set'), p.logs.join('\n'));
  assert.equal(p.ctx.patterns.take([]).meta.slug, other);
});

// A chain still samples the textures of a pattern a sketch is drawing, so
// setChanged() leaves it for release() to take, through sweepRetired().
test('a pattern a sketch is drawing outlives setChanged() and goes at release()', () => {
  const p = page([]);
  switchTo(p, 0);
  p.run(90000);
  const before = cached(p);
  assert.equal(before.length, 3);
  const drawn = p.ctx.patterns.take([]);
  p.ctx.patterns.bind(drawn, 0, 0);
  const line = 'visuals: pattern evicted ' + drawn.meta.slug + ' not in the visuals set';
  const outside = SLUGS.find(s => !before.includes(s));
  p.files['sets.json'] = setsFile([randomSet(7, [outside])]);
  assert.equal(switchTo(p, 7), 2);
  assert.ok(cached(p).includes(drawn.meta.slug), 'the drawn pattern was evicted');
  assert.ok(!p.logs.includes(line));
  p.ctx.patterns.release();
  assert.ok(p.logs.includes(line), p.logs.join('\n'));
  assert.ok(!cached(p).includes(drawn.meta.slug));
});

test('a set with no pattern ticked leaves ready() false', () => {
  const p = page([randomSet(1, [])]);
  switchTo(p, 1);
  p.run(90000);
  assert.deepEqual(loaded(p), []);
  assert.equal(p.ctx.patterns.ready(), false);
});

test('a set change with nothing to evict does not start a load before the 10 s boot delay', () => {
  const p = page([]);
  p.run(1000);                               // the first feed frame, about a second in
  assert.equal(switchTo(p, 0), 0);
  p.run(7500);                               // now about 8.6 s after load
  assert.deepEqual(p.xhrs.filter(u => u.startsWith('assets/patterns/')), []);
  p.run(10000);
  assert.ok(p.xhrs.some(u => u.startsWith('assets/patterns/')), 'no load at all after 18 s');
});
