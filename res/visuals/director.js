// Runs the show: sources, sketch rotation, the fps cap, the idle fallback and
// the numbers the load test reads from ~/bitedj-visuals.log.
//
// Sketches drive their own per-frame work through window.sketchUpdate(dt),
// which this file calls every rendered frame; a sketch must never assign
// window.update itself, that property belongs to this file (see the notes
// by the assignments below for why).
(function () {
  const RENDER_W = 960, RENDER_H = 540;
  const FPS = 30;
  const BEATS_PER_SWITCH = 256;     // 64 bars at 4/4
  const MIN_SKETCH_MS = 60000;
  const IDLE_AFTER_MS = 20000;      // no beat for this long: idle sketch
  const IDLE_ROTATE_MS = 180000;
  const DIP_MS = 125;               // half of the ~250ms total not-visible budget
  const LOG_EVERY_MS = 10000;

  const canvas = document.getElementById('stage');
  const hydra = new Hydra({ canvas, width: RENDER_W, height: RENDER_H, detectAudio: false, makeGlobal: true });
  // The width and height above do nothing when a canvas is supplied:
  // _initCanvas (vendor/hydra-synth.js:3154-3158) adopts canvas.width and
  // canvas.height instead and drops the options. #stage is sized in CSS only,
  // so without this the whole show renders at the canvas element's default
  // 300x150 and is stretched to the panel. setResolution is the only thing
  // that actually resizes the canvas and the four output framebuffers.
  hydra.setResolution(RENDER_W, RENDER_H);
  window.hydra = hydra;
  // makeGlobal mirrors window.fps onto synth.fps on every rendered frame
  // (EvalSandbox.tick() in the vendored bundle, vendor/hydra-synth.js
  // ~1084-1090), so window.fps is the only assignment that sticks; a direct
  // hydra.synth.fps write would be overwritten back to undefined (uncapped)
  // on the very next frame, so there is no synth.fps line here at all.
  window.fps = FPS;

  // Sources. s0 webcam, s1 the wordmark, s2 the boot logo.
  window.camReady = false;
  s0.initCam(0);
  // hydra resolves the camera asynchronously and swallows the rejection, so
  // probe it ourselves to know whether camera sketches are usable.
  navigator.mediaDevices.enumerateDevices().then(devs => {
    window.camReady = devs.some(d => d.kind === 'videoinput');
    console.log('visuals: camera', window.camReady ? 'present' : 'absent');
  }).catch(() => { window.camReady = false; });

  const mark = document.createElement('canvas');
  mark.width = 1024; mark.height = 256;
  (function drawWordmark() {
    const ctx = mark.getContext('2d');
    ctx.clearRect(0, 0, mark.width, mark.height);
    ctx.font = 'bold 150px "DejaVu Sans", "Liberation Sans", Arial, sans-serif';
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.lineWidth = 4; ctx.strokeStyle = '#ffffff';
    ctx.shadowColor = '#ffffff'; ctx.shadowBlur = 24;
    ctx.strokeText('BIG TIMBER', mark.width / 2, mark.height / 2);
    ctx.shadowBlur = 0;
    ctx.strokeText('BIG TIMBER', mark.width / 2, mark.height / 2);
  })();
  s1.init({ src: mark, dynamic: false });
  s2.initImage('assets/boot-logo.png');

  // Rotation.
  let current = null, currentSince = 0, lastBeatAt = performance.now(), idle = false, lastIdleSwitch = 0;
  let history = [];
  // setTimeout id of a dip in flight, so a second show() call before the
  // first one lands restarts the wait instead of racing it.
  let pendingSwitch = null;

  // window.update, not hydra.synth.update: same makeGlobal mirroring as
  // fps above (EvalSandbox.tick() copies window.update onto synth.update
  // every frame), so this is the assignment that has to stick. It both
  // counts frames for the fps log and drives whatever the current sketch
  // has put on window.sketchUpdate.
  let frames = 0;
  function driveFrame(dt) {
    frames += 1;
    if (typeof window.sketchUpdate === 'function') {
      try { window.sketchUpdate(dt); } catch (e) { console.error('visuals: sketchUpdate failed', e); }
    }
  }

  // hush() (vendor/hydra-synth.js:3054-3065) also calls source.clear() on
  // every source (s0-s3): that stops the webcam's tracks and replaces the
  // wordmark/boot-logo textures (s1/s2) with 1x1 blanks, so every camera or
  // logo sketch would come up blank after the very first switch. This does
  // the safe subset of what hush() does: it clears the four outputs (what a
  // sketch actually draws into) without touching the sources.
  function clearOutputs() {
    solid(0, 0, 0, 0).out(o0);
    solid(0, 0, 0, 0).out(o1);
    solid(0, 0, 0, 0).out(o2);
    solid(0, 0, 0, 0).out(o3);
    render(o0);
  }

  function eligible() {
    return window.sketches.filter(s => !s.cam || window.camReady);
  }

  function pickNext() {
    const list = eligible();
    if (!list.length) return window.idleSketch;
    const last = history[history.length - 1];
    let candidates = list.filter(s => s !== last);
    if (last) {
      // Step 1: prefer a sketch from the other family (colour vs monochrome).
      const otherFamily = candidates.filter(s => s.mono !== last.mono);
      if (otherFamily.length) candidates = otherFamily;
      // Step 2: avoid two logotype sketches back to back, when an alternative exists.
      if (last.name.startsWith('logo')) {
        const notLogo = candidates.filter(s => !s.name.startsWith('logo'));
        if (notLogo.length) candidates = notLogo;
      }
    }
    if (!candidates.length) candidates = list;
    return candidates[Math.floor(Math.random() * candidates.length)];
  }

  function show(sketch) {
    if (!sketch) { console.log('visuals: no sketch to show'); return; }
    if (pendingSwitch) { clearTimeout(pendingSwitch); pendingSwitch = null; }
    canvas.classList.add('dip');
    pendingSwitch = setTimeout(() => {
      pendingSwitch = null;
      clearOutputs();
      window.update = driveFrame;
      feed.clearBeatListeners();
      window.sketchUpdate = null;
      try {
        sketch.run();
      } catch (e) {
        console.error('visuals: sketch failed', sketch && sketch.name, e);
      }
      canvas.classList.remove('dip');
      current = sketch; currentSince = performance.now();
      history.push(sketch); if (history.length > 8) history.shift();
      console.log('visuals: sketch', sketch.name);
    }, DIP_MS);
  }

  // The rotation logic must survive sketch switches, so it subscribes with
  // onBeatAlways; onBeat is sketch-scoped and gets wiped by show() above.
  feed.onBeatAlways(() => {
    lastBeatAt = performance.now();
    if (idle) { idle = false; show(pickNext()); return; }
    if (feed.beats % BEATS_PER_SWITCH === 0 && performance.now() - currentSince > MIN_SKETCH_MS) {
      show(pickNext());
    }
  });

  setInterval(() => {
    const now = performance.now();
    if (!idle && now - lastBeatAt > IDLE_AFTER_MS) {
      idle = true; lastIdleSwitch = now; show(window.idleSketch);
    } else if (idle && now - lastIdleSwitch > IDLE_ROTATE_MS) {
      lastIdleSwitch = now; show(window.idleSketch);
    }
  }, 1000);

  // Evidence for the load test.
  (function logRenderer() {
    const gl = canvas.getContext('webgl2') || canvas.getContext('webgl');
    const ext = gl && gl.getExtension('WEBGL_debug_renderer_info');
    console.log('visuals: renderer', ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : 'unknown');
  })();
  let lastLog = performance.now();
  window.update = driveFrame;
  setInterval(() => {
    const now = performance.now();
    console.log('visuals: fps', (frames * 1000 / (now - lastLog)).toFixed(1), 'feed', feed.alive ? 'alive' : 'dead', 'bpm', feed.bpm.toFixed(1));
    frames = 0; lastLog = now;
  }, LOG_EVERY_MS);

  show(window.idleSketch);
  idle = true; lastIdleSwitch = performance.now();
})();
