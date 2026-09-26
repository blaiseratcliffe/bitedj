// director.js walking a Sequence set: entry order, per-entry bars, the
// pattern hold, Next, idle, video entries, switching in and out.
//
// Run from bitedj/:  node --test res/visuals/tests/director-sequence.test.js
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { loadPage, setsFile, fakePatterns } = require('./page-rig.js');

const KNOBS = { bars: 16, reactivity: 1, bounce: 2, swirl: 2, camMix: 1 };
const SKETCHES = [
  { name: 'logo-kaleid', cam: false, mono: true, run() {} },
  { name: 'tunnel', cam: false, mono: false, run() {} },
  { name: 'contours', cam: false, mono: true, run() {} },
  { name: 'pattern-flow', cam: false, mono: true, pattern: true, run() {} },
  { name: 'video-kaleid', cam: false, mono: false, video: true, run() {} },
  { name: 'cam-edges', cam: true, mono: true, run() {} }
];

// A video stub: records open() calls, in `calls` too when given; every clip
// listed exists, and once anything is opened a clip is open.
function fakeVideo(files, calls = null) {
  const opened = [];
  return {
    opened,
    clips: () => files.map(file => ({ file, seconds: 60, src: 'x/' + file, source: 'deployed' })),
    failed: () => [],
    available: () => files.length > 0,
    isOpen: () => opened.length > 0,
    open(file) { opened.push(file || null); if (calls) calls.push('video.open(' + (file || '') + ')'); },
    close() {},
    refresh(done) { if (done) done(); },
    onGiveUp() {},
    file: () => null
  };
}

function seqPage(entries, opts = {}) {
  const seq = { id: 2, name: 'Opener', type: 'sequence', entries, knobs: KNOBS };
  const rnd = { id: 1, name: 'Random', type: 'random', sketches: ['tunnel', 'contours', 'logo-kaleid'], patterns: [], clips: [], knobs: KNOBS };
  return loadPage({
    sketches: SKETCHES,
    settings: Object.assign({ bars: 1, set: 2 }, opts.settings),
    files: { 'sets.json': setsFile([seq, rnd]) },
    patterns: opts.patterns || fakePatterns(['arcs_1', 'grid_2'], ['arcs_1', 'grid_2']),
    video: opts.video || fakeVideo(['a.mp4']),
    calls: opts.calls || null
  });
}

// The sequence switch lines, 'visuals: set 2 "Opener" entry i/n sketch ...'.
const entryLines = (p) => p.logs.filter(l => /^visuals: set \d+ "[^"]*" entry \d+\/\d+ /.test(l));
// 'entry i/n sketch' from each.
const entries = (p) => entryLines(p).map(l => l.replace(/^visuals: set \d+ "[^"]*" /, '').split(' ').slice(0, 3).join(' '));

// Frames until `pred` holds, up to `ms`; true if it did.
function until(p, pred, ms = 10000) {
  const end = p.now + ms;
  while (p.now < end) { if (pred()) return true; p.frame(); }
  return pred();
}

test('the sequence plays intro once, then loops, and logs each entry', () => {
  const p = seqPage([{ sketch: 'logo-kaleid', intro: true }, { sketch: 'tunnel' }, { sketch: 'contours' }]);
  p.run(3000);
  p.beats(10);                                 // leave idle: entry 1
  p.beats(200);
  const seen = entries(p);
  assert.ok(seen.length >= 5, 'only ' + seen.length + ' entry lines');
  assert.deepEqual(seen.slice(0, 5), ['entry 1/3 logo-kaleid', 'entry 2/3 tunnel', 'entry 3/3 contours', 'entry 2/3 tunnel', 'entry 3/3 contours']);
  assert.ok(p.logs.includes('visuals: set 2 "Opener" entry 1/3 logo-kaleid'), p.logs.join('\n'));
});

test('an entry lasts its own bars, else the Bars knob, with the 8 s floor', () => {
  // bars 4 on entry 2 is 16 beats; the knob (bars 1) is 4 beats but the
  // floor makes it about 24 beats at 345 ms.
  const p = seqPage([{ sketch: 'tunnel' }, { sketch: 'contours', bars: 4 }], { settings: { bars: 1 } });
  p.run(3000);
  p.beats(1);
  const beatsOn = (name) => {
    let n = 0;
    while (p.current().name !== name && n < 400) { p.beats(1); n += 1; }
    const start = p.ctx.feed.beats;
    while (p.current().name === name && n < 800) { p.beats(1); n += 1; }
    return p.ctx.feed.beats - start;
  };
  const contours = beatsOn('contours');
  const tunnel = beatsOn('tunnel');
  assert.ok(contours >= 23 && contours <= 30, 'contours lasted ' + contours + ' beats');
  assert.ok(tunnel >= 23 && tunnel <= 30, 'tunnel lasted ' + tunnel + ' beats (the 8 s floor)');
  // Now with a length above the floor.
  const p2 = seqPage([{ sketch: 'tunnel' }, { sketch: 'contours', bars: 16 }]);
  p2.run(3000);
  p2.beats(1);
  let n = 0;
  while (p2.current().name !== 'contours' && n < 400) { p2.beats(1); n += 1; }
  const start = p2.ctx.feed.beats;
  while (p2.current().name === 'contours' && n < 1000) { p2.beats(1); n += 1; }
  const lasted = p2.ctx.feed.beats - start;
  assert.ok(lasted >= 64 && lasted <= 70, '16 bars lasted ' + lasted + ' beats');
});

test('a pattern entry pins its pattern, and the next pattern is prefetched', () => {
  const pats = fakePatterns(['arcs_1', 'grid_2'], ['arcs_1']);
  const p = seqPage([{ sketch: 'pattern-flow', pattern: 'arcs_1' }, { sketch: 'tunnel' }, { sketch: 'pattern-flow', pattern: 'grid_2' }], { patterns: pats });
  p.run(3000);
  p.beats(1);
  p.run(3000);
  assert.equal(p.current().name, 'pattern-flow');
  assert.ok(pats.pins.includes('arcs_1'));
  p.beats(40);                                  // entry 2, tunnel, lands
  assert.equal(p.current().name, 'tunnel');
  assert.equal(pats.pinned(), 'grid_2', 'the next entry\'s pattern was not prefetched');
  assert.ok(p.logs.includes('visuals: set 2 "Opener" entry 1/3 pattern-flow arcs_1'));
});

// The pin moves at the landing: after the still of the outgoing frame, just
// before the entry's sketch runs, and never while the entry is only armed.
test('a pattern entry\'s pattern is pinned when it lands, not when it is asked for', () => {
  const calls = [];
  const pats = fakePatterns(['arcs_1'], ['arcs_1'], calls);
  const p = seqPage([{ sketch: 'tunnel' }, { sketch: 'pattern-flow', pattern: 'arcs_1' }], { patterns: pats, calls });
  p.run(3000);
  p.beats(1);
  p.run(3000);
  assert.equal(p.current().name, 'tunnel');
  let n = 0;
  while (p.current().name !== 'pattern-flow' && n < 200) { p.beats(1); n += 1; }
  assert.equal(p.current().name, 'pattern-flow');
  const lastStill = calls.lastIndexOf('s3.subimage');
  const lastPin = calls.lastIndexOf('pin(arcs_1)');
  assert.ok(lastStill >= 0 && lastPin > lastStill, calls.slice(-8).join(' '));
});

test('a pattern that is late holds the current entry, then goes when it lands', () => {
  const pats = fakePatterns(['arcs_1'], []);
  const p = seqPage([{ sketch: 'tunnel' }, { sketch: 'pattern-flow', pattern: 'arcs_1' }], { patterns: pats });
  p.run(3000);
  p.beats(1);
  p.run(3000);
  assert.equal(p.current().name, 'tunnel');
  p.beats(40);                                  // due, but arcs_1 is still loading
  assert.equal(p.current().name, 'tunnel');
  assert.ok(p.logs.includes('visuals: holding for pattern arcs_1'), p.logs.join('\n'));
  pats.finishLoads();
  p.beats(2);
  p.run(3000);
  assert.equal(p.current().name, 'pattern-flow');
});

test('a pattern still missing after 8 bars of holding is skipped with a log line', () => {
  const pats = fakePatterns(['arcs_1'], []);
  const p = seqPage([{ sketch: 'tunnel' }, { sketch: 'pattern-flow', pattern: 'arcs_1' }, { sketch: 'contours' }], { patterns: pats });
  p.run(3000);
  p.beats(1);
  p.run(3000);
  let n = 0;
  while (p.current().name === 'tunnel' && n < 200) { p.beats(1); n += 1; }
  assert.equal(p.current().name, 'contours');
  assert.ok(p.logs.includes('visuals: sets 2 entry 2/3 pattern-flow skipped, pattern arcs_1 not loaded after 8 bars'), p.logs.join('\n'));
  assert.ok(p.logs.includes('visuals: holding for pattern arcs_1'));
});

test('Next steps to the next entry at once, skipping an unloaded pattern', () => {
  const pats = fakePatterns(['arcs_1'], []);
  const p = seqPage([{ sketch: 'tunnel' }, { sketch: 'pattern-flow', pattern: 'arcs_1' }, { sketch: 'contours' }], { patterns: pats });
  p.run(3000);
  p.beats(1);
  p.run(3000);
  assert.equal(p.current().name, 'tunnel');
  p.ctx.feed.settings.next += 1;
  p.run(3000);
  assert.equal(p.current().name, 'contours');
  assert.ok(p.logs.includes('visuals: sets 2 entry 2/3 pattern-flow skipped, pattern arcs_1 not loaded'), p.logs.join('\n'));
});

test('after idle the interrupted entry starts again', () => {
  const p = seqPage([{ sketch: 'tunnel' }, { sketch: 'contours' }, { sketch: 'logo-kaleid' }]);
  p.run(3000);
  p.beats(1);
  p.beats(40);
  const on = p.current().name;
  p.run(25000);                                 // no beats: idle
  assert.equal(p.current().name, 'idle-contours');
  p.beat();
  p.run(3000);
  assert.equal(p.current().name, on);
  const last = entryLines(p).pop();
  assert.ok(last && last.endsWith(' ' + on), 'the resumed entry was not logged as one: ' + last);
});

test('a video entry opens its clip from the start', () => {
  const vid = fakeVideo(['a.mp4', 'b.mp4']);
  const p = seqPage([{ sketch: 'video-kaleid', clip: 'b.mp4' }, { sketch: 'tunnel' }], { video: vid });
  p.run(3000);
  p.beats(1);
  p.run(3000);
  assert.equal(p.current().name, 'video-kaleid');
  assert.ok(vid.opened.includes('b.mp4'), vid.opened.join(' '));
  assert.ok(p.logs.includes('visuals: set 2 "Opener" entry 1/2 video-kaleid b.mp4'));
});

// The second clip must not replace the first before the melt's still of the
// first has been taken, or the melt starts from a blank or the new clip.
test('a video entry after a video entry opens its clip after the still is taken', () => {
  const calls = [];
  const vid = fakeVideo(['a.mp4', 'b.mp4'], calls);
  const p = seqPage([{ sketch: 'video-kaleid', clip: 'a.mp4' }, { sketch: 'video-kaleid', clip: 'b.mp4' }], { video: vid, calls });
  p.run(3000);
  p.beats(1);
  p.run(3000);
  let n = 0;
  while (!p.logs.some(l => l.includes(' entry 2/2 ')) && n < 200) { p.beats(1); n += 1; }
  const a = calls.indexOf('video.open(a.mp4)');
  const b = calls.indexOf('video.open(b.mp4)');
  assert.ok(a >= 0 && b > a, calls.join(' '));
  // Two stills between them: entry 1's landing, then entry 2's.
  const stills = calls.slice(a, b).filter(c => c === 's3.subimage').length;
  assert.equal(stills, 2, calls.slice(a, b + 1).join(' '));
});

test('entries that cannot play are skipped: a camera sketch without a camera, a retired pattern, a missing clip', () => {
  const pats = fakePatterns(['arcs_1'], ['arcs_1']);
  pats.setStatus('arcs_1', 'retired');
  const p = seqPage([
    { sketch: 'cam-edges' },
    { sketch: 'pattern-flow', pattern: 'arcs_1' },
    { sketch: 'video-kaleid', clip: 'gone.mp4' },
    { sketch: 'tunnel' }
  ], { patterns: pats });
  p.run(3000);
  p.beats(200);
  const seen = entries(p);
  assert.ok(seen.length >= 3, 'only ' + seen.length + ' entry lines');
  seen.forEach(e => assert.equal(e, 'entry 4/4 tunnel'));
});

test('a too-solid pattern still plays when an entry names it', () => {
  const pats = fakePatterns(['arcs_1'], ['arcs_1']);
  pats.setStatus('arcs_1', 'too solid');
  const p = seqPage([{ sketch: 'pattern-flow', pattern: 'arcs_1' }, { sketch: 'tunnel' }], { patterns: pats });
  p.run(3000);
  p.beats(100);
  assert.ok(entries(p).includes('entry 1/2 pattern-flow'), entries(p).join(', '));
});

test('the Patterns row off skips pattern entries', () => {
  const p = seqPage([{ sketch: 'pattern-flow', pattern: 'arcs_1' }, { sketch: 'tunnel' }], { settings: { patterns: 0 } });
  p.run(3000);
  p.beats(100);
  const seen = entries(p);
  assert.ok(seen.length >= 3, 'only ' + seen.length + ' entry lines');
  seen.forEach(e => assert.equal(e, 'entry 2/2 tunnel'));
});

test('nothing that can play shows the idle sketch, and says so once', () => {
  const p = seqPage([{ sketch: 'cam-edges' }]);
  p.run(3000);
  p.beats(100);
  p.run(3000);
  assert.equal(p.current().name, 'idle-contours');
  assert.equal(p.logs.filter(l => l === 'visuals: sets 2 has nothing that can play').length, 1, p.logs.join('\n'));
});

test('switching into a sequence melts to entry 1 on the next beat', () => {
  const p = seqPage([{ sketch: 'logo-kaleid', intro: true }, { sketch: 'contours' }], { settings: { set: 1 } });
  p.run(3000);
  p.beats(10);
  p.ctx.feed.settings.set = 2;
  p.run(500);
  p.beats(2);
  p.run(3000);
  assert.equal(p.current().name, 'logo-kaleid');
  assert.ok(p.logs.includes('visuals: set 2 "Opener" entry 1/2 logo-kaleid'), p.logs.join('\n'));
});

test('switching into a sequence whose first pattern is not loaded pins it at once, holds, then starts on entry 1', () => {
  const pats = fakePatterns(['arcs_1'], []);
  const p = seqPage([{ sketch: 'pattern-flow', pattern: 'arcs_1' }, { sketch: 'tunnel' }], { patterns: pats, settings: { set: 1 } });
  p.run(3000);
  p.beats(10);
  const before = p.current().name;
  p.ctx.feed.settings.set = 2;
  p.run(500);
  assert.ok(pats.pins.includes('arcs_1'), 'entry 1\'s pattern was not asked for at the switch');
  p.beats(8);
  assert.equal(p.current().name, before);
  pats.finishLoads();
  p.beats(2);
  p.run(3000);
  assert.ok(entries(p)[0] === 'entry 1/2 pattern-flow', entries(p).join(', '));
});

test('leaving a sequence unpins its pattern and follows the Random rule', () => {
  const pats = fakePatterns(['arcs_1'], ['arcs_1']);
  const p = seqPage([{ sketch: 'pattern-flow', pattern: 'arcs_1' }, { sketch: 'tunnel' }], { patterns: pats });
  p.run(3000);
  p.beats(1);
  p.run(3000);
  assert.equal(p.current().name, 'pattern-flow');
  p.ctx.feed.settings.set = 1;
  p.run(3000);
  assert.equal(pats.pinned(), null);
  assert.ok(['tunnel', 'contours', 'logo-kaleid'].includes(p.current().name), 'still on ' + p.current().name);
});

test('an edit of the playing sequence leaves the screen alone', () => {
  const p = seqPage([{ sketch: 'tunnel' }, { sketch: 'contours' }]);
  p.run(3000);
  p.beats(1);
  p.run(3000);
  const on = p.current().name;
  const seq = { id: 2, name: 'Opener', type: 'sequence', entries: [{ sketch: 'tunnel' }, { sketch: 'contours' }, { sketch: 'logo-kaleid' }], knobs: KNOBS };
  p.files['sets.json'] = setsFile([seq]);
  p.ctx.feed.settings.setRev = 1;
  p.run(3000);
  assert.equal(p.current().name, on);
});

// Review fix round 1 from here on.

// Beats until the director has armed a switch to `name` that has not landed
// yet (show() has run, startPending() has not); how many beats that took.
function armUntil(p, name, max = 400) {
  const d = p.ctx.director;
  for (let n = 0; n < max; n++) {
    p.beat();
    if (d.target() !== d.current() && (!name || d.target().name === name)) return n;
    p.run(345);
  }
  return max;
}

// Finding 1: an entry armed when the set switches away used to land
// afterwards and pin its pattern for good, under a set that never named it.
test('an entry still landing when the show leaves the sequence lands as a plain sketch and pins nothing', () => {
  const pats = fakePatterns(['arcs_1', 'grid_2'], ['arcs_1', 'grid_2']);
  const seq = { id: 2, name: 'Opener', type: 'sequence', entries: [{ sketch: 'tunnel' }, { sketch: 'pattern-flow', pattern: 'arcs_1' }], knobs: KNOBS };
  const rnd = { id: 1, name: 'Random', type: 'random', sketches: ['tunnel', 'contours', 'pattern-flow'], patterns: ['grid_2'], clips: [], knobs: KNOBS };
  const p = loadPage({ sketches: SKETCHES, settings: { bars: 1, set: 2 }, files: { 'sets.json': setsFile([seq, rnd]) }, patterns: pats, video: fakeVideo([]) });
  p.run(3000);
  p.beats(1);
  p.run(3000);
  assert.ok(armUntil(p, 'pattern-flow') < 400, 'the pattern entry was never armed');
  p.ctx.feed.settings.set = 1;                 // before it lands
  p.run(5000);
  assert.equal(p.current().name, 'pattern-flow');
  assert.equal(pats.pinned(), null, 'the stale entry pinned its pattern under the Random set');
  assert.ok(!p.logs.some(l => l.startsWith('visuals: set 1 "Random" entry ')), p.logs.filter(l => /^visuals: set \d/.test(l)).join('\n'));
  p.beats(200);
  assert.equal(pats.pinned(), null);
});

// Finding 1 again: sequence A's stale entry landing under sequence B used to
// answer B's switch, so B's entry 1 waited a whole entry rather than a beat.
test('switching between sequences while an entry is landing still starts the new one on the next beat', () => {
  const a = { id: 2, name: 'Opener', type: 'sequence', entries: [{ sketch: 'tunnel' }, { sketch: 'contours' }], knobs: KNOBS };
  const b = { id: 3, name: 'Closer', type: 'sequence', entries: [{ sketch: 'logo-kaleid' }, { sketch: 'contours' }], knobs: KNOBS };
  const p = loadPage({ sketches: SKETCHES, settings: { bars: 1, set: 2 }, files: { 'sets.json': setsFile([a, b]) },
    patterns: fakePatterns([], []), video: fakeVideo([]) });
  p.run(3000);
  p.beats(1);
  p.run(3000);
  assert.ok(armUntil(p, 'contours') < 400, 'entry 2 was never armed');
  p.ctx.feed.settings.set = 3;
  p.run(500);
  p.beats(8);
  p.run(3000);
  assert.equal(p.current().name, 'logo-kaleid');
  assert.ok(p.logs.includes('visuals: set 3 "Closer" entry 1/2 logo-kaleid'), p.logs.filter(l => /^visuals: set \d/.test(l)).join('\n'));
  assert.ok(!p.logs.includes('visuals: set 3 "Closer" entry 2/2 contours'), 'Opener\'s entry landed as one of Closer\'s');
});

// Finding 2: the first beat out of idle used to skip an entry whose pattern
// was still loading, with no line, and an intro skipped is gone for good.
test('switching into a sequence while idle, with entry 1\'s pattern still loading, shows entry 1 on the first beat after it loads', () => {
  const pats = fakePatterns(['arcs_1'], []);
  const p = seqPage([{ sketch: 'pattern-flow', pattern: 'arcs_1', intro: true }, { sketch: 'tunnel' }, { sketch: 'contours' }],
    { patterns: pats, settings: { set: 1 } });
  p.run(3000);                                  // idle from boot, no beats
  p.ctx.feed.settings.set = 2;
  p.run(1000);
  p.beat();                                     // out of idle; arcs_1 still loading
  p.run(3000);
  assert.equal(p.current().name, 'idle-contours');
  assert.ok(p.logs.includes('visuals: holding for pattern arcs_1'), p.logs.join('\n'));
  pats.finishLoads();
  p.beats(2);
  p.run(3000);
  assert.equal(p.current().name, 'pattern-flow');
  assert.equal(entries(p)[0], 'entry 1/3 pattern-flow', entries(p).join(', '));
});

test('a resume whose pattern never loads holds 8 bars, then says so and moves on', () => {
  const pats = fakePatterns(['arcs_1'], []);
  const p = seqPage([{ sketch: 'pattern-flow', pattern: 'arcs_1', intro: true }, { sketch: 'tunnel' }, { sketch: 'contours' }],
    { patterns: pats, settings: { set: 1 } });
  p.run(3000);
  p.ctx.feed.settings.set = 2;
  p.run(1000);
  p.beats(20);
  assert.equal(p.current().name, 'idle-contours', 'the resume did not hold');
  p.beats(20);
  p.run(3000);
  assert.ok(p.logs.includes('visuals: sets 2 entry 1/3 pattern-flow skipped, pattern arcs_1 not loaded after 8 bars'), p.logs.join('\n'));
  assert.equal(entries(p)[0], 'entry 2/3 tunnel');
});

test('idle keeps the interrupted entry\'s pattern pinned, and that entry resumes', () => {
  const pats = fakePatterns(['arcs_1', 'grid_2'], ['arcs_1', 'grid_2']);
  const p = seqPage([{ sketch: 'tunnel' }, { sketch: 'pattern-flow', pattern: 'arcs_1' }, { sketch: 'pattern-flow', pattern: 'grid_2' }], { patterns: pats });
  p.run(3000);
  p.beats(1);
  let n = 0;
  while (p.current().name !== 'pattern-flow' && n < 200) { p.beats(1); n += 1; }
  p.run(3000);
  assert.equal(pats.pinned(), 'grid_2', 'the next entry\'s pattern was not prefetched');
  p.run(25000);                                 // no beats: idle
  assert.equal(p.current().name, 'idle-contours');
  assert.equal(pats.pinned(), 'arcs_1', 'idle let go of the interrupted entry\'s pattern');
  p.beat();
  p.run(3000);
  assert.equal(entryLines(p).pop(), 'visuals: set 2 "Opener" entry 2/3 pattern-flow arcs_1');
});

// Finding 8: one hold per switch. The second unloaded entry in a row is
// skipped at once, with the short line, not held for another 8 bars.
test('two unloaded pattern entries in a row cost one hold, not two', () => {
  const pats = fakePatterns(['arcs_1', 'grid_2'], []);
  const p = seqPage([{ sketch: 'tunnel' }, { sketch: 'pattern-flow', pattern: 'arcs_1' }, { sketch: 'pattern-flow', pattern: 'grid_2' }, { sketch: 'contours' }],
    { patterns: pats });
  p.run(3000);
  p.beats(1);
  p.run(3000);
  let n = 0;
  while (p.current().name === 'tunnel' && n < 200) { p.beats(1); n += 1; }
  assert.equal(p.current().name, 'contours');
  assert.ok(p.logs.includes('visuals: sets 2 entry 2/4 pattern-flow skipped, pattern arcs_1 not loaded after 8 bars'), p.logs.join('\n'));
  assert.ok(p.logs.includes('visuals: sets 2 entry 3/4 pattern-flow skipped, pattern grid_2 not loaded'), p.logs.join('\n'));
});

// Finding 5: a switch into a sequence that drops a Random sketch before it
// lands holds for entry 1's pattern, like any due switch, rather than
// skipping entry 1 on the spot.
test('a switch that drops a landing sketch holds for entry 1\'s pattern', () => {
  const pats = fakePatterns(['arcs_1'], []);
  const seq = { id: 2, name: 'Opener', type: 'sequence', entries: [{ sketch: 'pattern-flow', pattern: 'arcs_1' }, { sketch: 'tunnel' }], knobs: KNOBS };
  const rnd = { id: 1, name: 'Random', type: 'random', sketches: ['contours', 'logo-kaleid'], patterns: [], clips: [], knobs: KNOBS };
  const p = loadPage({ sketches: SKETCHES, settings: { bars: 1, set: 1 }, files: { 'sets.json': setsFile([seq, rnd]) }, patterns: pats, video: fakeVideo([]) });
  p.run(3000);
  p.beats(1);
  p.run(3000);
  assert.ok(armUntil(p, null) < 400, 'no switch was ever armed');
  p.ctx.feed.settings.set = 2;                 // lands between show() and startPending()
  p.run(1000);
  assert.ok(p.logs.some(l => / dropped (contours|logo-kaleid) before it landed$/.test(l)), p.logs.join('\n'));
  assert.ok(!p.logs.some(l => / entry 1\/2 pattern-flow skipped/.test(l)), p.logs.join('\n'));
  pats.finishLoads();
  p.beats(2);
  p.run(3000);
  assert.equal(entries(p)[0], 'entry 1/2 pattern-flow', entries(p).join(', '));
});

// Finding 6: leaving a sequence with nothing to play for a Random set does
// not leave the idle picture up until the next due switch.
test('leaving a sequence that had nothing to play shows the new set at once', () => {
  const p = seqPage([{ sketch: 'cam-edges' }]);
  p.run(3000);
  p.beats(100);
  p.run(3000);
  assert.equal(p.current().name, 'idle-contours');
  p.ctx.feed.settings.set = 1;
  p.run(3000);
  assert.ok(['tunnel', 'contours', 'logo-kaleid'].includes(p.current().name), 'still on ' + p.current().name);
});
