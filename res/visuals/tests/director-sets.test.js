// director.js with a Random set in force: the rotation stays inside it,
// a set switch melts away only from a sketch the new set leaves out, and
// every switch is logged with the set it happened under.
//
// Run from bitedj/:  node --test res/visuals/tests/director-sets.test.js
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { loadPage, setsFile, plainSketches } = require('./page-rig.js');

const KNOBS = { bars: 16, reactivity: 1, bounce: 2, swirl: 2, camMix: 1 };
const NAMES = ['a', 'b', 'c', 'd', 'e', 'f'];
const THREE = { id: 1, name: 'Three', type: 'random', sketches: ['a', 'b', 'c'], patterns: [], clips: [], knobs: KNOBS };
const OTHER = { id: 2, name: 'Other', type: 'random', sketches: ['d', 'e', 'f'], patterns: [], clips: [], knobs: KNOBS };

// bars 1 is 4 beats a switch; the director's 8 s floor then decides the pace.
function page(settings, sets = [THREE, OTHER]) {
  return loadPage({
    sketches: plainSketches(NAMES),
    settings: Object.assign({ bars: 1 }, settings),
    files: { 'sets.json': setsFile(sets) }
  });
}

test('the rotation stays inside a Random set', () => {
  const p = page({ set: 1 });
  p.run(3000);
  p.beats(400);
  const shown = p.shown().filter(n => n !== 'idle-contours');
  assert.ok(shown.length >= 10, 'expected at least 10 switches, got ' + shown.length);
  shown.forEach(n => assert.ok(['a', 'b', 'c'].includes(n), 'showed ' + n + ' outside the set'));
});

test('every switch names the set it happened under', () => {
  const p = page({ set: 1 });
  p.run(3000);
  p.beats(60);
  const lines = p.logs.filter(l => l.startsWith('visuals: set 1 "Three" '));
  assert.ok(lines.length >= 1, p.logs.join('\n'));
  assert.ok(!p.logs.some(l => l.startsWith('visuals: sketch ')), 'the old switch line is still logged');
});

test('Everything is logged as set 0', () => {
  const p = page({ set: 0 });
  p.run(3000);
  p.beats(60);
  assert.ok(p.logs.some(l => /^visuals: set 0 "Everything" [a-f]$/.test(l)), p.logs.join('\n'));
});

// 'visuals: set <id> ' starts a switch line and nothing else; what the change
// handler says starts 'visuals: sets ', so counting switches never counts it.
test('a set change is logged as a sets line, not as a switch', () => {
  const p = page({ set: 1 });
  p.run(3000);
  p.beats(30);
  p.ctx.feed.settings.set = 2;
  p.run(3000);
  assert.ok(p.logs.includes('visuals: sets 1 "Three" in force (switch)'), p.logs.join('\n'));
  assert.ok(p.logs.includes('visuals: sets 2 "Other" in force (switch)'), p.logs.join('\n'));
  assert.ok(p.logs.some(l => /^visuals: sets 2 leaves out [abc]$/.test(l)), p.logs.join('\n'));
  const switches = p.logs.filter(l => /^visuals: set \d+ /.test(l));
  assert.ok(switches.length >= 2);
  switches.forEach(l => assert.ok(/^visuals: set \d+ "[^"]*" [a-f]$/.test(l) || / idle-contours$/.test(l), l));
});

test('switching to a set that leaves out the sketch on screen melts away from it', () => {
  const p = page({ set: 1 });
  p.run(3000);
  p.beats(30);
  const before = p.current().name;
  assert.ok(['a', 'b', 'c'].includes(before));
  p.ctx.feed.settings.set = 2;
  p.run(3000);                              // no beats: only the set change can move it
  assert.ok(['d', 'e', 'f'].includes(p.current().name), 'still on ' + p.current().name);
});

test('switching to a set that keeps the sketch on screen leaves it there', () => {
  const both = { id: 3, name: 'Both', type: 'random', sketches: NAMES.slice(), patterns: [], clips: [], knobs: KNOBS };
  const p = page({ set: 1 }, [THREE, both]);
  p.run(3000);
  p.beats(30);
  const before = p.current().name;
  p.ctx.feed.settings.set = 3;
  p.run(3000);
  assert.equal(p.current().name, before);
});

test('an edit that drops the sketch on screen melts away from it', () => {
  const p = page({ set: 1 });
  p.run(3000);
  p.beats(30);
  const on = p.current().name;
  const edited = Object.assign({}, THREE, { sketches: ['a', 'b', 'c', 'd'].filter(n => n !== on) });
  p.files['sets.json'] = setsFile([edited, OTHER]);
  p.ctx.feed.settings.setRev = 1;
  p.run(3000);
  assert.notEqual(p.current().name, on);
});

test('an unreadable file plays everything and melts nothing', () => {
  const p = page({ set: 1 });
  p.run(3000);
  p.beats(30);
  const on = p.current().name;
  p.files['sets.json'] = '{"version": 1, "sets": [';
  p.ctx.feed.settings.setRev = 1;
  p.run(3000);
  assert.equal(p.current().name, on);
  assert.ok(p.logs.some(l => l.startsWith('visuals: sets unreadable')));
  p.beats(300);
  const later = p.shown().slice(-20);
  assert.ok(later.some(n => ['d', 'e', 'f'].includes(n)), 'never left the old set: ' + later.join(' '));
});

test('a set change while a melt is in flight lands on an allowed sketch', () => {
  const p = page({ set: 1 });
  p.run(3000);
  p.beats(30);
  p.ctx.director.show(p.ctx.sketches.find(s => s.name === 'a'));
  p.frame();                                // the snapshot is armed, the melt not yet landed
  p.ctx.feed.settings.set = 2;
  p.run(6000);
  assert.ok(['d', 'e', 'f'].includes(p.current().name), 'landed on ' + p.current().name);
});

// The test above is met by judging `current` alone: the read lands a frame
// after the switch does. Here the sketch on screen stays allowed and only the
// one still being landed is left out, so only a handler that judges where
// the show is heading (queued, then pending, then current) moves it.
test('a set change that arrives while a switch is landing judges the incoming sketch', () => {
  const p = page({ set: 1 });
  p.run(3000);
  p.beats(30);
  const on = p.current().name;
  const incoming = ['a', 'b', 'c'].find(n => n !== on);
  const keep = { id: 4, name: 'Keep', type: 'random', sketches: [on, 'd', 'e', 'f'], patterns: [], clips: [], knobs: KNOBS };
  p.files['sets.json'] = setsFile([THREE, OTHER, keep]);
  p.ctx.director.show(p.ctx.sketches.find(s => s.name === incoming));
  p.ctx.feed.settings.set = 4;
  p.run(6000);
  assert.ok(p.logs.includes('visuals: sets 4 leaves out ' + incoming), p.logs.join('\n'));
  assert.ok([on, 'd', 'e', 'f'].includes(p.current().name), 'landed on ' + p.current().name);
});

// A video and a pattern loader stub that record what the director asks of
// them. There is one clip, x.mp4, and available() follows the set in force,
// as video.js's does.
function stubs(holder, events) {
  let open = false;
  const allows = (kind, key) => !holder.page || holder.page.ctx.sets.allows(kind, key);
  return {
    video: {
      available: () => allows('clip', 'x.mp4'),
      isOpen: () => open,
      open() { open = true; events.push('video open'); },
      close() { open = false; events.push('video close'); },
      refresh() { events.push('video refresh'); },
      onGiveUp() {}
    },
    patterns: {
      ready: () => false,
      release() {},
      setChanged() { events.push('patterns setChanged'); return 0; }
    }
  };
}

// The handler's pick is queued behind the switch still landing, and that
// switch is to a sketch the new set leaves out. It must not land: here it is
// a video sketch and the new set has no clip, so startPending() would close
// the clip and melt the sketch in over a blank s8.
test('a sketch the new set leaves out never lands, so a video sketch never melts in over a closed clip', () => {
  const holder = {};
  const events = [];
  const s = stubs(holder, events);
  const A = { id: 1, name: 'A', type: 'random', sketches: ['a', 'b', 'c', 'vk'], patterns: [], clips: ['x.mp4'], knobs: KNOBS };
  const B = { id: 2, name: 'B', type: 'random', sketches: ['d', 'e', 'f'], patterns: [], clips: [], knobs: KNOBS };
  const sketches = plainSketches(NAMES).concat([{ name: 'vk', cam: false, mono: false, video: true, run() {} }]);
  const p = holder.page = loadPage({
    sketches, settings: { bars: 1, set: 1 }, files: { 'sets.json': setsFile([A, B]) },
    video: s.video, patterns: s.patterns
  });
  p.run(3000);
  p.beats(30);
  p.run(2500);                              // no beats: a melt in flight ends, so show() arms at once
  p.ctx.director.show(p.ctx.sketches.find(x => x.name === 'vk'));
  p.ctx.feed.settings.set = 2;
  p.run(6000);
  assert.ok(p.logs.includes('visuals: sets 2 leaves out vk'), p.logs.join('\n'));
  assert.ok(!p.logs.includes('visuals: set 2 "B" vk'), 'vk landed under set 2:\n' + p.logs.join('\n'));
  assert.ok(['d', 'e', 'f'].includes(p.current().name), 'landed on ' + p.current().name);
});

// The third place a left-out sketch can be: queued behind a melt, with the
// sketch on screen still allowed. The handler has to see it and say so.
test('a left-out sketch waiting behind a melt is replaced by the handler', () => {
  const p = page({ set: 1 });
  p.run(3000);
  p.beats(30);
  p.run(2500);
  const on = p.current().name;
  const [y, z] = ['a', 'b', 'c'].filter(n => n !== on);
  const keep = { id: 4, name: 'Keep', type: 'random', sketches: [y, 'd', 'e', 'f'], patterns: [], clips: [], knobs: KNOBS };
  p.files['sets.json'] = setsFile([THREE, OTHER, keep]);
  const find = (n) => p.ctx.sketches.find(x => x.name === n);
  p.ctx.director.show(find(y));
  p.run(100);                               // grabbed, landed, melting
  assert.equal(p.current().name, y);
  p.ctx.director.show(find(z));             // held in queued behind the melt
  assert.equal(p.ctx.director.target().name, z);
  p.ctx.feed.settings.set = 4;
  p.run(6000);
  assert.ok(p.logs.includes('visuals: sets 4 leaves out ' + z), p.logs.join('\n'));
  assert.ok(!p.logs.includes('visuals: set 4 "Keep" ' + z), z + ' landed under set 4');
  assert.ok([y, 'd', 'e', 'f'].includes(p.current().name), 'landed on ' + p.current().name);
});

// Decision 17: the uploads are reread on every set change, and the pattern
// cache is told, for a switch and for an edit alike.
test('every set change, switch or edit, rereads the uploads and tells the pattern loader', () => {
  const holder = {};
  const events = [];
  const s = stubs(holder, events);
  const p = holder.page = loadPage({
    sketches: plainSketches(NAMES), settings: { bars: 1, set: 1 },
    files: { 'sets.json': setsFile([THREE, OTHER]) }, video: s.video, patterns: s.patterns
  });
  const count = (e) => events.filter(x => x === e).length;
  p.run(1000);
  assert.deepEqual([count('video refresh'), count('patterns setChanged')], [1, 1]);
  p.ctx.feed.settings.set = 2;
  p.run(1000);
  assert.deepEqual([count('video refresh'), count('patterns setChanged')], [2, 2]);
  p.ctx.feed.settings.setRev = 5;
  p.run(1000);
  assert.deepEqual([count('video refresh'), count('patterns setChanged')], [3, 3]);
});

test('the set is read only once the feed is alive', () => {
  const p = page({ set: 1 });
  p.ctx.feed.alive = false;
  p.run(3000);
  assert.ok(!p.logs.some(l => l.startsWith('visuals: set 1 ')));
  p.ctx.feed.alive = true;
  p.run(3000);
  p.beats(30);
  assert.ok(p.logs.some(l => l.startsWith('visuals: set 1 "Three"')));
});
