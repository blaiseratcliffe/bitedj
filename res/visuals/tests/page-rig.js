// A Node rig for the HDMI page's own scripts. Loads the real director.js
// (and sets.js, video.js, patterns.js if asked) into a vm context where
// hydra, the DOM, the feed and the clock are stubs, so the switch logic can
// be driven frame by frame and beat by beat without a GPU.
//
//   const page = loadPage({ sketches, settings, files, scripts });
//   page.run(3000);          // 3 s of 30 fps frames
//   page.beat();             // one beat from the feed
//   page.logs                // every console line, joined with spaces
//
// Nothing is drawn. Every hydra function returns a chain proxy that accepts
// any property and any call, which is all director.js needs of it. Timers,
// animation frames and Math.random are fake: timers and frames fire only as
// frame() advances the clock, and Math.random is a seeded generator, so a
// test that depends on a pick fails or passes the same way every run.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const ROOT = path.join(__dirname, '..');

function chain() {
  const fn = function () { return proxy; };
  const proxy = new Proxy(fn, {
    get: (t, k) => (k === 'then' ? undefined : proxy),
    apply: () => proxy
  });
  return proxy;
}

// mulberry32: small, fast and the same on every machine.
function seeded(seed) {
  let a = seed >>> 0;
  return function () {
    a = (a + 0x6D2B79F5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// A <video> element that records what it is asked and never loads anything.
function videoStub() {
  const el = {
    readyState: 0, currentTime: 0, duration: NaN, error: null, style: {},
    addEventListener() {}, removeEventListener() {},
    pause() { el.paused = true; },
    play() { el.paused = false; return Promise.resolve(); },
    load() {},
    removeAttribute(k) { delete el[k]; }
  };
  return el;
}

function source(name, calls) {
  return {
    tex: { subimage() { calls.push(name + '.subimage'); }, destroy() {} },
    init() { calls.push(name + '.init'); },
    initCam(i) { calls.push(name + '.initCam(' + i + ')'); },
    initImage() { calls.push(name + '.initImage'); },
    initVideo(u) { calls.push(name + '.initVideo(' + u + ')'); },
    clear() { calls.push(name + '.clear'); }
  };
}

function loadPage(opts) {
  const o = Object.assign({
    sketches: [],
    settings: {},
    files: {},                       // path -> text, served by visualsPaths.readText
    scripts: ['sets.js', 'director.js'],
    patterns: null,                  // a stub object, or null for none; ignored when patterns.js is loaded
    video: null,                     // a stub object, or null for none; ignored when video.js is loaded
    videoIndex: null,                // window.videoIndex for video.js
    patternIndex: null,              // window.patternIndex for patterns.js
    // For patterns.js: what an XHR of `url` answers. A { status, text }
    // object, null for a network error, or 'hang' for no answer at all.
    xhr: null,
    // For patterns.js: milliseconds the clock moves while a pattern frame is
    // drawn, by slug, which is how a test makes a load cost too much.
    drawCost: null,
    calls: null,                     // an array to record calls into, shared with a stub
    search: '',
    seed: 1
  }, opts);

  let now = 0;
  let nextId = 1;
  const timers = [];
  const frames = [];                 // requestAnimationFrame callbacks due next frame
  const logs = [];
  const calls = o.calls || [];
  const beatFns = [];
  const elements = [];
  const storage = {};
  const blobs = {};
  const xhrs = [];

  const ctx = {};
  ctx.window = ctx;
  ctx.console = {
    log: (...a) => logs.push(a.map(String).join(' ')),
    error: (...a) => logs.push('ERROR ' + a.map(String).join(' ')),
    warn: (...a) => logs.push('WARN ' + a.map(String).join(' '))
  };
  ctx.performance = { now: () => now };
  ctx.setTimeout = (fn, ms) => { const id = nextId++; timers.push({ at: now + (ms || 0), fn, id }); return id; };
  ctx.setInterval = (fn, ms) => { const id = nextId++; timers.push({ at: now + ms, every: ms, fn, id }); return id; };
  ctx.clearTimeout = ctx.clearInterval = (id) => {
    const i = timers.findIndex(t => t.id === id);
    if (i >= 0) timers.splice(i, 1);
  };
  ctx.requestAnimationFrame = (fn) => { const id = nextId++; frames.push({ fn, id }); return id; };
  ctx.cancelAnimationFrame = (id) => {
    const i = frames.findIndex(f => f.id === id);
    if (i >= 0) frames.splice(i, 1);
  };
  ctx.Promise = Promise;
  ctx.JSON = JSON;
  ctx.Math = Object.assign(Object.create(Math), { random: seeded(o.seed) });
  ctx.Uint8ClampedArray = Uint8ClampedArray;
  ctx.Uint32Array = Uint32Array;
  ctx.location = { protocol: 'file:', search: o.search, origin: 'null' };
  ctx.URLSearchParams = URLSearchParams;
  ctx.navigator = {};
  ctx.localStorage = {
    getItem: (k) => (Object.prototype.hasOwnProperty.call(storage, k) ? storage[k] : null),
    setItem: (k, v) => { storage[k] = String(v); }
  };

  // Canvases: a 2d context whose drawImage moves the clock by the drawn
  // pattern's cost, and whose readback is all black.
  function slugOf(img) {
    const text = img && blobs[img.src];
    const m = text && /data-slug="([^"]+)"/.exec(text);
    return m ? m[1] : null;
  }
  function canvasStub() {
    const known = {
      drawImage(img) {
        const slug = slugOf(img);
        if (slug && o.drawCost && o.drawCost[slug]) now += o.drawCost[slug];
      },
      getImageData(x, y, w, h) { return { data: new Uint8ClampedArray(w * h * 4) }; },
      fillRect() {}, clearRect() {}, strokeText() {}, fillText() {}
    };
    const c2d = new Proxy(known, {
      get: (t, k) => (k in t ? t[k] : () => {}),
      set: (t, k, v) => { t[k] = v; return true; }
    });
    return { width: 300, height: 150, style: {}, getContext: (kind) => (kind === '2d' ? c2d : null) };
  }

  ctx.Blob = function (parts) { this.text = parts.join(''); };
  ctx.URL = {
    createObjectURL(b) { const u = 'blob:' + (nextId++); blobs[u] = b.text; return u; },
    revokeObjectURL(u) { delete blobs[u]; }
  };
  ctx.Image = function () { this.onload = null; this.onerror = null; this._src = ''; };
  Object.defineProperty(ctx.Image.prototype, 'src', {
    get() { return this._src; },
    set(v) {
      this._src = v;
      if (!v) return;
      ctx.setTimeout(() => { if (this._src === v && this.onload) this.onload(); }, 0);
    }
  });
  ctx.XMLHttpRequest = function () { this.status = 0; this.responseText = ''; this.onload = null; this.onerror = null; };
  ctx.XMLHttpRequest.prototype.open = function (method, url) { this.method = method; this.url = url; };
  ctx.XMLHttpRequest.prototype.overrideMimeType = function () {};
  ctx.XMLHttpRequest.prototype.send = function () {
    xhrs.push(this.url);
    const answer = o.xhr ? o.xhr(this.url, this.method) : null;
    if (answer === 'hang') return;
    ctx.setTimeout(() => {
      if (!answer) { if (this.onerror) this.onerror(); return; }
      this.status = answer.status;
      this.responseText = answer.text || '';
      if (this.onload) this.onload();
    }, 0);
  };

  ctx.document = {
    getElementById: () => canvasStub(),
    createElement: (tag) => {
      const el = tag === 'canvas' ? canvasStub() : tag === 'video' ? videoStub() : { style: {}, appendChild() {} };
      elements.push({ tag, el });
      return el;
    },
    head: { appendChild() {} },
    body: { appendChild() {}, style: {} }
  };
  ctx.Hydra = function () { return { setResolution() {} }; };
  ['src', 'solid', 'noise', 'osc', 'shape', 'gradient', 'voronoi', 'render'].forEach((n) => { ctx[n] = chain(); });
  ['o0', 'o1', 'o2', 'o3'].forEach((n) => { ctx[n] = { name: n }; });
  for (let i = 0; i <= 8; i++) ctx['s' + i] = source('s' + i, calls);

  ctx.feed = {
    settings: Object.assign({
      reactivity: 1, bounce: 2, swirl: 2, bars: 16, camMix: 1, camSketches: 1,
      patterns: 1, next: 0, set: 0, setRev: 0
    }, o.settings),
    alive: true, playing: true, beats: 0, bpm: 174,
    rawBass: 0, energy: 0, swell: 0, pulse: 0, bounce: 0,
    onBeat() {},
    onBeatAlways(fn) { beatFns.push(fn); },
    clearBeatListeners() {}
  };
  ctx.visualsPaths = {
    media: 'media/',
    setsFile: 'sets.json',
    // Asynchronous like the XHR: the answer arrives on the next frame.
    readText(p, cb) {
      const text = Object.prototype.hasOwnProperty.call(o.files, p) ? o.files[p] : null;
      ctx.setTimeout(() => cb(text === null ? new Error('missing or empty') : null, text), 0);
    }
  };
  if (o.patterns && !o.scripts.includes('patterns.js')) ctx.patterns = o.patterns;
  if (o.video && !o.scripts.includes('video.js')) ctx.video = o.video;
  if (o.videoIndex) ctx.videoIndex = o.videoIndex;
  if (o.patternIndex) ctx.patternIndex = o.patternIndex;
  ctx.sketches = o.sketches;
  ctx.idleSketch = { name: 'idle-contours', cam: false, mono: true, run() {} };

  vm.createContext(ctx);
  o.scripts.forEach((f) => {
    vm.runInContext(fs.readFileSync(path.join(ROOT, f), 'utf8'), ctx, { filename: f });
  });

  function runTimers() {
    for (;;) {
      let due = null;
      timers.forEach((t) => { if (t.at <= now && (!due || t.at < due.at)) due = t; });
      if (!due) return;
      if (due.every) due.at += due.every;
      else timers.splice(timers.indexOf(due), 1);
      due.fn();
    }
  }

  // The switch line: 'visuals: set <id> "<name>" <sketch>', or for a
  // sequence '... entry <i>/<n> <sketch> [pattern] [clip]'. Nothing else in
  // the page starts 'visuals: set <digits> '.
  const SWITCH = /^visuals: set \d+ "(?:[^"\\]|\\.)*" (?:entry \d+\/\d+ )?(\S+)/;

  const page = {
    ctx, logs, calls, elements, storage, xhrs, files: o.files,
    get now() { return now; },
    // One rendered frame: timers due by now fire, then the animation frame
    // callbacks queued before this frame, then hydra's window.update(dt), the
    // draw, and window.afterUpdate().
    frame(dt = 33) {
      now += dt;
      runTimers();
      frames.splice(0).forEach(f => f.fn(now));
      if (typeof ctx.update === 'function') ctx.update(dt);
      if (typeof ctx.afterUpdate === 'function') ctx.afterUpdate();
    },
    run(ms, dt = 33) {
      const end = now + ms;
      while (now < end) page.frame(dt);
    },
    beat() {
      ctx.feed.beats += 1;
      beatFns.forEach(fn => fn());
    },
    // `count` beats, `every` ms apart, frames in between.
    beats(count, every = 345) {
      for (let i = 0; i < count; i++) { page.beat(); page.run(every); }
    },
    current() { return ctx.director.current(); },
    // Every sketch the director has landed on, in order, from the switch lines.
    shown() {
      return logs.map(l => SWITCH.exec(l)).filter(Boolean).map(m => m[1]);
    }
  };
  return page;
}

// A sets file with the given sets.
function setsFile(sets) {
  return JSON.stringify({ version: 1, nextId: 100, known: { sketches: [], patterns: [], clips: [] }, sets });
}

// Plain sketches with no flags; `mono` alternates so the family rule has
// something to alternate.
function plainSketches(names) {
  return names.map((name, i) => ({ name, cam: false, mono: i % 2 === 0, run() {} }));
}

// A patternIndex of `slugs`, one tag each, seven small frames, and an xhr
// answer that serves any of their frames. `hang` lists slugs whose first
// frame never answers.
function patternFixture(slugs, hang = []) {
  const index = slugs.map((slug, i) => ({
    slug, title: slug, tags: [['GRID', 'FLOW', 'NOISE', 'RADIAL'][i % 4]],
    shapes: 1, bytes: 700, frame: 100, wire: false, param: 'p',
    frames: [0, 1, 2, 3, 4, 5].map(k => slug + '/p-' + k + '.svg').concat([slug + '.svg'])
  }));
  const xhr = (url) => {
    const m = /assets\/patterns\/([^/.]+)/.exec(url);
    if (!m) return null;
    if (hang.includes(m[1])) return 'hang';
    return { status: 0, text: '<svg data-slug="' + m[1] + '" viewBox="0 0 10 10"></svg>' };
  };
  return { index, xhr };
}

module.exports = { loadPage, setsFile, plainSketches, patternFixture };
