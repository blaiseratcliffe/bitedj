// director.useCamera(): preview mode's stand-in replaces the webcam. Once
// installed, camera sketches are eligible, show() opens and closes the
// stand-in instead of s0.initCam/s0.clear, and a late camera probe cannot
// turn camReady back off. Also the idle exit under pause: a sketch preview
// asked for survives the first beat.
//
// Run from bitedj/:  node --test res/visuals/tests/director-camera.test.js
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { loadPage } = require('./page-rig.js');

const SKETCHES = [
  { name: 'cam-edges', cam: true, mono: true, run() {} },
  { name: 'tunnel', cam: false, mono: false, camMix: 'cut', run() {} },
  { name: 'contours', cam: false, mono: true, run() {} }
];

function page() {
  return loadPage({ sketches: SKETCHES, settings: { bars: 1 }, files: {} });
}

test('without a camera, camera sketches never show', () => {
  const p = page();
  p.run(3000);
  p.beats(300);
  assert.ok(!p.shown().includes('cam-edges'));
});

test('useCamera makes camera sketches eligible and routes open and close through it', () => {
  const p = page();
  const used = [];
  p.ctx.director.useCamera({ label: 'stand-in', open() { used.push('open'); }, close() { used.push('close'); } });
  assert.equal(p.ctx.camReady, true);
  p.run(3000);
  p.ctx.director.show(SKETCHES[0]);
  p.run(3000);
  assert.equal(p.current().name, 'cam-edges');
  assert.deepEqual(used, ['open']);
  p.ctx.director.show(SKETCHES[2]);
  p.run(3000);
  assert.deepEqual(used, ['open', 'close']);
  assert.ok(!p.calls.some(c => c.startsWith('s0.initCam')), p.calls.join(' '));
});

test('a camMix sketch opens the stand-in when the Mix row is on', () => {
  const p = page();
  const used = [];
  p.ctx.director.useCamera({ label: 'stand-in', open() { used.push('open'); }, close() { used.push('close'); } });
  p.run(3000);
  p.ctx.director.show(SKETCHES[1]);
  p.run(3000);
  assert.deepEqual(used, ['open']);
});

// cam-edges, with no camera, is a sketch pickNext() can never return, so if
// the first beat's random pick replaces the one preview asked for, the test
// sees it every time rather than only when the pick happens to differ.
test('paused, a sketch asked for during the boot melt survives the first beat', () => {
  const p = page();
  p.ctx.director.pause(true);
  p.ctx.director.show(SKETCHES[0]);          // queued behind the boot melt
  p.beat();                                  // first beat, still idle
  p.run(3000);
  p.beats(10);
  assert.equal(p.current().name, 'cam-edges');
  assert.deepEqual(p.shown(), ['idle-contours', 'cam-edges']);
});

test('unpaused, the first beat still leaves idle with a random pick', () => {
  const p = page();
  p.run(3000);
  p.beat();
  p.run(3000);
  assert.notEqual(p.current().name, 'idle-contours');
});
