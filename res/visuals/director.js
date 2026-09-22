// Runs the show: sources, sketch rotation, the fps cap, the idle fallback and
// the numbers the load test reads from ~/bitedj-visuals.log.
(function () {
  const RENDER_W = 960, RENDER_H = 540;
  const FPS = 30;
  const BEATS_PER_SWITCH = 256;     // 64 bars at 4/4
  const MIN_SKETCH_MS = 60000;
  const IDLE_AFTER_MS = 20000;      // no beat for this long: idle sketch
  const IDLE_ROTATE_MS = 180000;
  const DIP_MS = 250;
  const LOG_EVERY_MS = 10000;

  const canvas = document.getElementById('stage');
  const hydra = new Hydra({ canvas, width: RENDER_W, height: RENDER_H, detectAudio: false, makeGlobal: true });
  window.hydra = hydra;
  // makeGlobal mirrors window.fps onto synth.fps on every rendered frame
  // (EvalSandbox.tick() in the vendored bundle), so setting synth.fps alone
  // is clobbered back to undefined on the next frame, uncapping the loop.
  window.fps = FPS;
  hydra.synth.fps = FPS;

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

  // hush() (called on every sketch switch) resets update to a no-op via
  // sandbox.set, and because makeGlobal is true, EvalSandbox.tick() copies
  // window.update onto synth.update on every rendered frame — so it is
  // window.update that must carry the frame counter, and it must be
  // reinstalled after each hush(), not just registered once.
  let frames = 0;
  function countFrame() { frames += 1; }

  function eligible() {
    return window.sketches.filter(s => !s.cam || window.camReady);
  }

  function pickNext() {
    const list = eligible();
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
    return candidates[Math.floor(Math.random() * candidates.length)] || list[0];
  }

  function show(sketch) {
    canvas.classList.add('dip');
    setTimeout(() => {
      hush();
      window.update = countFrame;
      try { sketch.run(); } catch (e) { console.error('visuals: sketch failed', sketch.name, e); }
      current = sketch; currentSince = performance.now();
      history.push(sketch); if (history.length > 8) history.shift();
      console.log('visuals: sketch', sketch.name);
      canvas.classList.remove('dip');
    }, DIP_MS);
  }

  feed.onBeat(() => {
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
  window.update = countFrame;
  setInterval(() => {
    const now = performance.now();
    console.log('visuals: fps', (frames * 1000 / (now - lastLog)).toFixed(1), 'feed', feed.alive ? 'alive' : 'dead', 'bpm', feed.bpm.toFixed(1));
    frames = 0; lastLog = now;
  }, LOG_EVERY_MS);

  show(window.idleSketch);
  idle = true; lastIdleSwitch = performance.now();
})();
