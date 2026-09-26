// patterns.js's pin loader outside preview mode, which a sequence uses on the
// Pi: a hung load is given up after the prewarm's 60 s, a load is measured
// into the cost record, and one over twice the frame budget is dropped. In
// preview mode none of that changes. The last test holds the prewarm's own
// watchdog to its log line, since Step 12 moves it into watchLoad().
//
// Run from bitedj/:  node --test res/visuals/tests/patterns-pin.test.js
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { loadPage, patternFixture } = require('./page-rig.js');

const SLUGS = ['p1', 'p2', 'p3'];
const COST_KEY = 'bitedj.patterns.cost';

function page(opts = {}) {
  const fx = patternFixture(SLUGS, opts.hang || []);
  return loadPage({
    scripts: ['patterns.js'],
    patternIndex: fx.index,
    xhr: fx.xhr,
    drawCost: opts.drawCost || null,
    search: opts.search || ''
  });
}

const status = (p, slug) => p.ctx.patterns.library().find(x => x.slug === slug).status;
const costOf = (p, slug) => {
  const raw = p.storage[COST_KEY];
  return raw ? JSON.parse(raw)[slug] : undefined;
};

test('a pin load that hangs is given up after 60 s, and the next pin still loads', () => {
  const p = page({ hang: ['p1'] });
  const got = [];
  p.ctx.patterns.pin('p1', (e) => got.push(e ? e.meta.slug : null));
  p.run(61000);
  assert.deepEqual(got, [null]);
  assert.ok(p.logs.includes('visuals: pattern load timed out p1 for the sequence, skipped for this session'), p.logs.join('\n'));
  assert.equal(p.ctx.patterns.pinned(), null);
  assert.equal(status(p, 'p1'), 'skipped this session');
  p.ctx.patterns.pin('p2', (e) => got.push(e ? e.meta.slug : null));
  p.run(5000);
  assert.deepEqual(got, [null, 'p2']);
  assert.ok(p.logs.some(l => l.startsWith('visuals: pattern ready p2 for the sequence,')), p.logs.join('\n'));
});

test('a pin load within budget writes a cost record', () => {
  const p = page({ drawCost: { p2: 50 } });
  let got;
  p.ctx.patterns.pin('p2', (e) => { got = e; });
  p.run(5000);
  assert.ok(got, 'the pin never loaded');
  const rec = costOf(p, 'p2');
  assert.ok(rec, 'no cost record for p2');
  assert.equal(rec.over, 0);
  assert.equal(rec.worstMs, 50);
});

test('a pin load over twice the budget is recorded, dropped and skipped', () => {
  const p = page({ drawCost: { p3: 300 } });
  const got = [];
  p.ctx.patterns.pin('p3', (e) => got.push(e ? e.meta.slug : null));
  p.run(8000);
  assert.deepEqual(got, [null]);
  assert.equal(costOf(p, 'p3').over, 1);
  assert.equal(p.ctx.patterns.pinned(), null);
  assert.equal(status(p, 'p3'), 'skipped this session');
  assert.ok(!p.ctx.patterns.library().find(x => x.slug === 'p3').cached);
});

test('in preview mode a pin keeps its old behaviour: no record, no drop, no watchdog', () => {
  const p = page({ drawCost: { p3: 300 }, hang: ['p1'], search: '?preview=1' });
  let got;
  p.ctx.patterns.pin('p3', (e) => { got = e; });
  p.run(8000);
  assert.ok(got && got.meta.slug === 'p3');
  assert.equal(costOf(p, 'p3'), undefined);
  assert.ok(p.logs.some(l => l.startsWith('visuals: pattern ready p3 for the preview pin,')), p.logs.join('\n'));
  const hung = [];
  p.ctx.patterns.pin('p1', (e) => hung.push(e));
  p.run(65000);
  assert.deepEqual(hung, []);
});

// No pin at all: the prewarm's first load hangs, and its watchdog, which
// Step 12 moves into watchLoad(), still gives it up with the same line.
test('a prewarm load that hangs is still given up after 60 s, word for word as before', () => {
  const p = page({ hang: ['p1', 'p2', 'p3'] });
  p.run(75000);
  assert.ok(p.logs.some(l => /^visuals: pattern load timed out p\d skipped for this session$/.test(l)), p.logs.join('\n'));
});

// Review fix round 1 from here on.

// Finding 3: a sequence entry lands (release(), then its sketch's take()),
// and in the same tick the director pins the next entry's pattern. The
// pattern just taken is not `held` until the next frame binds it, and one
// outside the rotation (here over size) counts as dead weight, so the pin
// load used to evict it first and the sketch drew freed 1x1 canvases.
test('a pin never evicts what the sketch on screen has just taken, even outside the rotation', () => {
  const fx = patternFixture(['p1', 'p2', 'p3', 'p4']);
  fx.index[0].frame = 2000000;                  // p1 over size: outside the rotation
  const p = loadPage({ scripts: ['patterns.js'], patternIndex: fx.index, xhr: fx.xhr });
  const P = p.ctx.patterns;
  ['p2', 'p3', 'p1'].forEach((s) => { P.pin(s); p.run(2000); });
  assert.deepEqual(P.library().filter(x => x.cached).map(x => x.slug).sort(), ['p1', 'p2', 'p3']);
  P.release();                                  // the landing
  const got = P.take([]);
  assert.equal(got && got.meta.slug, 'p1');
  P.pin('p4');                                  // the next entry's prefetch
  p.run(3000);
  assert.ok(got.frames.every(c => c && c.width > 1), 'p1 was freed under the sketch drawing it');
  assert.ok(!p.logs.some(l => l.startsWith('visuals: pattern evicted p1')), p.logs.join('\n'));
  assert.ok(P.library().find(x => x.slug === 'p4').cached, 'p4 never loaded');
});

// Finding 4: a sequence pin whose load fails is marked, as the prewarm marks
// its own failures, so the director stops holding 8 bars for it every lap.
test('a pin load that fails outside preview mode is reported as failed to load', () => {
  const fx = patternFixture(SLUGS);
  const xhr = (url, m) => (/assets\/patterns\/p1/.test(url) ? null : fx.xhr(url, m));
  const p = loadPage({ scripts: ['patterns.js'], patternIndex: fx.index, xhr });
  const got = [];
  p.ctx.patterns.pin('p1', (e) => got.push(e ? e.meta.slug : null));
  p.run(5000);
  assert.deepEqual(got, [null]);
  assert.equal(status(p, 'p1'), 'failed to load');
});
