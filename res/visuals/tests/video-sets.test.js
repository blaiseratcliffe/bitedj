// video.js's clip list: the deployed index plus the uploads' index.json,
// narrowed by the active set, with pin() and open(file) choosing a clip.
//
// Run from bitedj/:  node --test res/visuals/tests/video-sets.test.js
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { loadPage, setsFile } = require('./page-rig.js');

const KNOBS = { bars: 16, reactivity: 1, bounce: 2, swirl: 2, camMix: 1 };
const DEPLOYED = [
  { file: 'changing-seasons.mp4', seconds: 101.02, width: 640, height: 360 },
  { file: 'mtb.mp4', seconds: 60, width: 640, height: 360 }
];
const UPLOADED = [
  { file: 'phone-clip.mp4', seconds: 12.5, width: 640, height: 360, poster: 'phone-clip.jpg' },
  { file: 'mtb.mp4', seconds: 5, width: 640, height: 360, poster: 'mtb.jpg' }
];

function page(files, settings = {}) {
  return loadPage({
    scripts: ['sets.js', 'video.js'],
    videoIndex: DEPLOYED,
    settings,
    files: Object.assign({ 'media/video/index.json': JSON.stringify(UPLOADED) }, files)
  });
}

// The video element video.js made: the one <video> in the rig.
const element = (p) => p.elements.find(e => e.tag === 'video').el;

test('the clip list merges deployed and uploaded, deployed winning a clash', () => {
  const p = page({});
  p.run(100);                                   // the first refresh lands
  const clips = p.ctx.video.clips();
  assert.deepEqual(clips.map(c => c.file), ['changing-seasons.mp4', 'mtb.mp4', 'phone-clip.mp4']);
  assert.equal(clips.find(c => c.file === 'mtb.mp4').source, 'deployed');
  assert.equal(clips.find(c => c.file === 'mtb.mp4').src, 'assets/video/mtb.mp4');
  assert.equal(clips.find(c => c.file === 'phone-clip.mp4').src, 'media/video/phone-clip.mp4');
  assert.equal(clips.find(c => c.file === 'phone-clip.mp4').source, 'uploaded');
});

test('no uploads index is not an error', () => {
  const p = loadPage({ scripts: ['sets.js', 'video.js'], videoIndex: DEPLOYED, files: {} });
  p.run(100);
  assert.equal(p.ctx.video.clips().length, 2);
  assert.ok(p.ctx.video.available());
  assert.ok(!p.logs.some(l => l.startsWith('ERROR')), p.logs.join('\n'));
});

test('an upload appears within 30 s without a reload', () => {
  const p = page({ 'media/video/index.json': '[]' });
  p.run(100);
  assert.equal(p.ctx.video.clips().length, 2);
  p.files['media/video/index.json'] = JSON.stringify(UPLOADED);
  p.run(30100);
  assert.equal(p.ctx.video.clips().length, 3);
});

test('refresh() rereads at once', () => {
  const p = page({ 'media/video/index.json': '[]' });
  p.run(100);
  p.files['media/video/index.json'] = JSON.stringify(UPLOADED);
  p.ctx.video.refresh();
  p.run(100);
  assert.equal(p.ctx.video.clips().length, 3);
});

// preview.js waits on this before showing a clip that may only be an upload.
test('refresh(done) calls done once the index has been read, found or not', () => {
  const p = page({});
  const order = [];
  p.ctx.video.refresh(() => order.push(p.ctx.video.clips().length));
  p.run(100);
  assert.deepEqual(order, [3]);
  delete p.files['media/video/index.json'];
  p.ctx.video.refresh(() => order.push(p.ctx.video.clips().length));
  p.run(100);
  assert.deepEqual(order, [3, 2]);
});

test('a Random set narrows what open() may pick, and available() follows', () => {
  const one = { id: 1, name: 'One clip', type: 'random', sketches: [], patterns: [], clips: ['phone-clip.mp4'], knobs: KNOBS };
  const none = { id: 2, name: 'No clips', type: 'random', sketches: [], patterns: [], clips: [], knobs: KNOBS };
  const p = page({ 'sets.json': setsFile([one, none]) });
  p.run(100);
  p.ctx.sets.update({ set: 1, setRev: 0 });
  p.run(100);
  for (let i = 0; i < 20; i++) {
    p.ctx.video.close();
    p.ctx.video.open();
    assert.equal(element(p).src, 'media/video/phone-clip.mp4');
  }
  p.ctx.sets.update({ set: 2, setRev: 0 });
  p.run(100);
  assert.equal(p.ctx.video.available(), false);
});

test('open(file) plays that clip even when one is already open', () => {
  const p = page({});
  p.run(100);
  p.ctx.video.open('changing-seasons.mp4');
  assert.equal(element(p).src, 'assets/video/changing-seasons.mp4');
  p.ctx.video.open('phone-clip.mp4');
  assert.equal(element(p).src, 'media/video/phone-clip.mp4');
});

test('pin(file) makes every plain open() use that clip; pin(null) lets go', () => {
  const p = page({});
  p.run(100);
  p.ctx.video.pin('mtb.mp4');
  for (let i = 0; i < 10; i++) {
    p.ctx.video.close();
    p.ctx.video.open();
    assert.equal(element(p).src, 'assets/video/mtb.mp4');
  }
  p.ctx.video.pin(null);
  const seen = new Set();
  for (let i = 0; i < 60; i++) {
    p.ctx.video.close();
    p.ctx.video.open();
    seen.add(element(p).src);
  }
  assert.ok(seen.size > 1, 'still pinned: ' + [...seen].join(' '));
});

test('open(file) of a clip that does not exist falls back to a random one, and says so', () => {
  const p = page({});
  p.run(100);
  p.ctx.video.open('gone.mp4');
  assert.ok(p.logs.some(l => l.includes('gone.mp4') && l.includes('not available')), p.logs.join('\n'));
  assert.ok(/\.mp4$/.test(element(p).src));
});

// Part B final review, finding 1. hydra's HydraSource.tick calls
// tex.subimage(el) every frame while s8 is dynamic. From a new src until the
// new clip has a frame, each call is 'texSubImage2D: no video', and Chromium
// stops reporting WebGL errors after 32 per page. This makes the rig's
// element and s8 behave that far: events fire when the test says, a new src
// or none drops the element to HAVE_NOTHING, and s8 keeps what init() gave it.
function live(p) {
  const el = element(p);
  const s8 = p.ctx.s8;
  const on = {};
  el.addEventListener = (ev, fn) => { (on[ev] = on[ev] || []).push(fn); };
  el.removeEventListener = (ev, fn) => { on[ev] = (on[ev] || []).filter(f => f !== fn); };
  let src;
  Object.defineProperty(el, 'src', {
    get: () => src,
    set: (v) => { src = v; el.readyState = 0; },
    configurable: true
  });
  el.removeAttribute = (k) => { if (k === 'src') { src = undefined; el.readyState = 0; } };
  s8.init = (o) => { s8.src = o.src; s8.dynamic = !!o.dynamic; };
  s8.clear = () => { s8.src = null; s8.dynamic = false; };
  const settle = () => new Promise(r => setImmediate(r));
  const fire = async (ev) => { (on[ev] || []).slice().forEach(fn => fn()); await settle(); };
  return {
    el, s8, fire,
    // What HydraSource.tick would do on this frame: true when it would call
    // subimage() on an element with no frame, the logged error.
    noVideo: () => !!s8.src && s8.dynamic === true && s8.src.readyState < 2,
    // The clip open() asked for decodes its first frame.
    async decode() {
      await fire('loadedmetadata');
      el.readyState = 2;
      await fire('loadeddata');
    }
  };
}

test('a clip opened over a playing one freezes s8 until the new clip has a frame', async () => {
  const p = page({});
  p.run(100);
  const v = live(p);
  p.ctx.video.open('changing-seasons.mp4');
  await v.decode();
  assert.equal(v.s8.src, v.el);
  assert.equal(v.s8.dynamic, true, 'the first clip never reached s8');
  p.ctx.video.open('phone-clip.mp4');           // a video entry after a playing clip
  assert.equal(v.el.src, 'media/video/phone-clip.mp4');
  assert.equal(v.noVideo(), false, 's8 still reads the element while its new src has no frame');
  await v.fire('loadedmetadata');
  assert.equal(v.noVideo(), false, 's8 reads the element before its first frame');
  v.el.readyState = 2;
  await v.fire('loadeddata');
  assert.equal(v.s8.src, v.el);
  assert.equal(v.s8.dynamic, true, 's8 did not go back to reading the new clip');
  assert.equal(p.ctx.video.file(), 'phone-clip.mp4');
});

test('a swapped-in clip that fails, and every other one after it, leaves s8 frozen, not reading an empty element', async () => {
  const p = page({});
  p.run(100);
  const v = live(p);
  p.ctx.video.open('changing-seasons.mp4');
  await v.decode();
  assert.equal(v.s8.dynamic, true);
  p.ctx.video.open('phone-clip.mp4');
  for (let i = 0; i < 5 && v.el.src; i++) {
    assert.equal(v.noVideo(), false, 's8 reads ' + v.el.src + ' before it has a frame');
    await v.fire('error');                      // this clip fails; the next is tried
    assert.equal(v.noVideo(), false, 's8 reads the element after a failed clip');
  }
  assert.equal(v.el.src, undefined, 'not every clip was tried');
  assert.ok(p.logs.some(l => l.includes('video unavailable')), p.logs.join('\n'));
  assert.notEqual(v.s8.dynamic, true);
});
