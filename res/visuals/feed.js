// The music signal. One EventSource on the engine's feed; smoothing, AGC,
// envelopes and beat-edge detection live here so sketches read plain 0..1
// numbers that never jump.
//
// Two clocks. Frames arrive at 30 Hz and set targets; everything a sketch
// reads is advanced on the render clock (requestAnimationFrame) toward those
// targets, so a 25 fps render never sees the 30 Hz staircase and a dropped
// network frame costs smoothness rather than a freeze.
//
// What a sketch should read, in order of how often it should be reached for:
//
//   feed.energy  bass with a 120 ms attack and a 700 ms release, mixed half
//                and half with a one bar running average. The "how loud is it
//                right now" number. Drive motion rates and warp amounts off
//                this.
//   feed.swell   bass averaged over four bars, about five and a half seconds
//                at 174. The "where are we in the track" number: this is what
//                a drop reads through, and it cannot be moved by one kick.
//   feed.pulse   1 on a beat edge, decaying to 0 with a time constant of a
//                third of a beat. The only instant signal in the file, and
//                the accents it drives should be worth a few percent, not a
//                slam.
//   feed.phase   0..1 through the current beat, free running on the render
//                clock and corrected forward from the master deck's
//                beat_distance. Continuous rotation locked to tempo.
//   feed.spring(name, target, stiffness)
//                a critically damped spring per name, so a beat can retarget
//                something and the picture eases there instead of cutting.
//
// The raw bands (bass, lowmid, mid, high, peak) are still here and still
// mirrored onto a.fft for community sketches, but they are the twitchiest
// thing in the file and sketches in this project should prefer the envelopes.
//
// Two beat-listener lists: onBeat(fn) is sketch-scoped and is wiped by
// clearBeatListeners() on every sketch switch (the director does this in
// show(), so a sketch never has to unsubscribe itself); onBeatAlways(fn) is
// permanent and is what the director's own rotation logic uses, so it keeps
// running across switches. Both lists fire on every beat edge.
// clearBeatListeners() also drops every spring, for the same reason: a sketch
// that comes round again should start from rest rather than from wherever it
// left its springs a quarter of an hour ago.
//
// ?mock=1 replaces the engine with a 174 BPM synthetic feed for desktop work.
(function () {
  const FEED_URL = 'http://127.0.0.1:7374/events';
  const BANDS = ['bass', 'lowmid', 'mid', 'high'];
  // Per incoming frame, and much gentler than they were: at ATTACK 0.6 the
  // bands landed on a full scale kick inside two frames and every sketch that
  // read feed.bass slammed with it.
  const ATTACK = 0.25;   // fraction of the way to a louder value per frame
  const RELEASE = 0.06;  // fraction of the way to a quieter value per frame
  const AGC_DECAY = 0.995; // running max decays slowly, so quiet passages still move
  const AGC_FLOOR = 0.02;
  const DEAD_AFTER_MS = 2000;
  const BEAT_VISIBLE_FRAMES = 2; // rAF ticks feed.beat stays true after an edge

  const ENERGY_ATTACK_MS = 120;
  const ENERGY_RELEASE_MS = 700;
  const EASE_MS = 60;        // render-clock approach to a 30 Hz target
  const PULSE_BEATS = 1 / 3; // pulse time constant, in beats
  const TARGET_BPM = 174;    // what the box is built for, and the bpm fallback
  const BARS_OF_SWELL = 4;
  const MAX_DT_MS = 100;     // a stall must not integrate as if it were real

  const feed = {
    bass: 0, lowmid: 0, mid: 0, high: 0, peak: 0,
    bpm: 0, beat: false, beats: 0, playing: false, alive: false,
    energy: 0, swell: 0, pulse: 0, phase: 0,
    _beatFns: [], _beatAlwaysFns: [], _beatFrames: 0,
    _lastFrameAt: 0, _max: [AGC_FLOOR, AGC_FLOOR, AGC_FLOOR, AGC_FLOOR],
    _prevBeat: [], _masterDeck: -1,
    // Targets set by ingest() at 30 Hz, chased by the render clock.
    _want: { bass: 0, lowmid: 0, mid: 0, high: 0, peak: 0, energy: 0, swell: 0 },
    _env: 0, _bar: 0, _swell: 0, _springs: Object.create(null),
    _renderAt: 0,
    onBeat(fn) { this._beatFns.push(fn); },
    onBeatAlways(fn) { this._beatAlwaysFns.push(fn); },
    clearBeatListeners() {
      this._beatFns.length = 0;
      this._springs = Object.create(null);
    },

    // Milliseconds per beat at the master deck's tempo. feed.bpm is 0 whenever
    // no deck is master, which is every start-up and every gap between a stop
    // and the idle fallback, so everything that divides by a beat comes
    // through here rather than touching feed.bpm itself.
    beatMs() {
      const bpm = this.bpm > 20 ? this.bpm : TARGET_BPM;
      return 60000 / bpm;
    },

    // A critically damped spring per name: it never overshoots and it never
    // jumps, which is the whole point. `target` is a number or a function of
    // no arguments, `stiffness` is in Hz, so 1.5 settles in about a second and
    // 4 in about a third of one. First call for a name starts at rest on the
    // target; after that the spring is advanced once per render frame in
    // renderTick() below, no matter how many times a chain reads it.
    spring(name, target, stiffness) {
      const want = typeof target === 'function' ? target() : target;
      let s = this._springs[name];
      if (!s) {
        s = this._springs[name] = { x: want, v: 0, want: want, hz: stiffness || 2 };
        return s.x;
      }
      s.want = want;
      if (stiffness) s.hz = stiffness;
      return s.x;
    }
  };
  window.feed = feed;
  window.a = {
    fft: [0, 0, 0, 0],
    setBins() {}, setSmooth() {}, setScale() {}, setCutoff() {}, show() {}, hide() {}
  };

  function smooth(prev, next) {
    return prev + (next > prev ? ATTACK : RELEASE) * (next - prev);
  }

  // Fraction of the way to a target for an exponential approach with time
  // constant `tau`, over `dt` milliseconds. Written out rather than hardcoded
  // per frame because both clocks here have a variable dt: the feed can drop a
  // frame and the render clock runs at whatever the compositor gives it.
  function approach(dt, tau) {
    return 1 - Math.exp(-dt / tau);
  }

  // Which deck the beat clock follows: playing, loudest, nudged toward the
  // crossfader side. Deck 1 and 3 are left, 2 and 4 right.
  function pickMaster(decks, xf) {
    let best = -1, bestScore = -1;
    decks.forEach((d, i) => {
      if (!d.play) return;
      const side = (i % 2 === 0) ? -1 : 1;
      const score = d.vu + 0.25 * Math.max(0, side * xf);
      if (score > bestScore) { best = i; bestScore = score; }
    });
    return best;
  }

  let lastIngestAt = 0;

  function ingest(frame) {
    const now = performance.now();
    const dt = lastIngestAt ? Math.min(MAX_DT_MS, now - lastIngestAt) : 1000 / 30;
    lastIngestAt = now;
    feed._lastFrameAt = now;
    feed.alive = true;

    let bassNorm = 0;
    BANDS.forEach((name, i) => {
      const raw = frame.bands[i] || 0;
      feed._max[i] = Math.max(raw, feed._max[i] * AGC_DECAY, AGC_FLOOR);
      const norm = Math.min(1, raw / feed._max[i]);
      feed._want[name] = smooth(feed._want[name], norm);
      if (i === 0) bassNorm = norm;
    });
    feed._want.peak = smooth(feed._want.peak, Math.min(1, frame.peak || 0));

    const decks = frame.decks || [];
    feed.playing = decks.some(d => d.play);
    feed._masterDeck = pickMaster(decks, frame.xf || 0);
    feed.bpm = feed._masterDeck >= 0 ? decks[feed._masterDeck].bpm : 0;

    // The energy envelope. Attack and release are in milliseconds rather than
    // per-frame fractions so they mean the same thing whatever the feed does,
    // and the release is six times the attack: the picture should arrive with
    // the kick and leave long after it.
    const tau = bassNorm > feed._env ? ENERGY_ATTACK_MS : ENERGY_RELEASE_MS;
    feed._env += (bassNorm - feed._env) * approach(dt, tau);

    // The bar average and the swell are exponential running averages rather
    // than ring buffers over the last N samples. Same mean, no discontinuity
    // when a sample falls out of the window, and no buffer to resize when the
    // tempo changes under them.
    const barMs = 4 * feed.beatMs();
    feed._bar += (bassNorm - feed._bar) * approach(dt, barMs);
    feed._swell += (bassNorm - feed._swell) * approach(dt, BARS_OF_SWELL * barMs);

    feed._want.energy = Math.max(0, Math.min(1, 0.5 * feed._env + 0.5 * feed._bar));
    feed._want.swell = Math.max(0, Math.min(1, feed._swell));

    // Only ever set feed.beat true here, on the rising edge. Clearing it is
    // the rAF housekeeping loop's job (below), so a sketch polling
    // feed.beat sees it for a full render frame regardless of how often
    // frames arrive over the network.
    let beatNow = false;
    decks.forEach((d, i) => {
      const active = d.beat > 0;   // 1 forward, 2 reverse; both are beats
      if (active && !feed._prevBeat[i] && i === feed._masterDeck) beatNow = true;
      feed._prevBeat[i] = active;
    });

    // Beat phase. The render clock runs it forward on its own (see
    // renderTick), and the deck's beat_distance only ever corrects it
    // forward, so a deck that reports a stale distance cannot drag the
    // rotation of a sketch backwards. A beat edge is the one place a
    // backward move is right, and there it is the wrap.
    const master = feed._masterDeck >= 0 ? decks[feed._masterDeck] : null;
    if (master && typeof master.bd === 'number') {
      const bd = master.bd - Math.floor(master.bd);
      if (beatNow) {
        feed.phase = bd;
      } else {
        const ahead = bd - feed.phase;
        if (ahead > 0 && ahead < 0.5) feed.phase = bd;
      }
    }

    if (beatNow) {
      feed.beat = true;
      feed._beatFrames = 0;
      feed.beats += 1;
      feed.pulse = 1;
      // Each listener is guarded on its own. The director's rotation logic
      // is the last entry in _beatAlwaysFns, so an exception thrown by a
      // sketch listener earlier in the pass would otherwise stop the show
      // from ever switching again.
      const fire = (fn) => {
        try { fn(); } catch (e) { console.error('visuals: beat listener failed', e); }
      };
      feed._beatFns.forEach(fire);
      feed._beatAlwaysFns.forEach(fire);
    }
  }

  // The render clock. Everything a sketch reads is finished here: the 30 Hz
  // targets are eased, the pulse decays, the phase free-runs and every spring
  // is advanced exactly once per frame. Clearing the one-frame beat flag and
  // the liveness flag also belongs here rather than in ingest(), so feed.beat
  // stays true for BEAT_VISIBLE_FRAMES rendered frames after an edge however
  // often frames arrive.
  function renderTick() {
    const now = performance.now();
    const dt = feed._renderAt ? Math.min(MAX_DT_MS, now - feed._renderAt) : 16;
    feed._renderAt = now;

    const k = approach(dt, EASE_MS);
    BANDS.forEach((name, i) => {
      feed[name] += (feed._want[name] - feed[name]) * k;
      window.a.fft[i] = feed[name];
    });
    feed.peak += (feed._want.peak - feed.peak) * k;
    feed.energy += (feed._want.energy - feed.energy) * k;
    feed.swell += (feed._want.swell - feed.swell) * k;

    const beatMs = feed.beatMs();
    feed.pulse *= Math.exp(-dt / (PULSE_BEATS * beatMs));
    if (feed.pulse < 1e-4) feed.pulse = 0;

    feed.phase += dt / beatMs;
    if (feed.phase >= 1) feed.phase -= Math.floor(feed.phase);

    // Exact solution of a critically damped spring over dt, so the step is
    // stable at any frame rate and the spring cannot ring or explode when the
    // compositor hands us a 100 ms frame.
    const secs = dt / 1000;
    const names = Object.keys(feed._springs);
    for (let i = 0; i < names.length; i++) {
      const s = feed._springs[names[i]];
      const w = 2 * Math.PI * s.hz;
      const e = Math.exp(-w * secs);
      const dx = s.x - s.want;
      const c = s.v + w * dx;
      s.x = s.want + (dx + c * secs) * e;
      s.v = (s.v - c * w * secs) * e;
    }

    if (feed.beat) {
      feed._beatFrames += 1;
      if (feed._beatFrames >= BEAT_VISIBLE_FRAMES) feed.beat = false;
    }
    feed.alive = now - feed._lastFrameAt < DEAD_AFTER_MS;
    requestAnimationFrame(renderTick);
  }
  requestAnimationFrame(renderTick);

  const mock = new URLSearchParams(location.search).get('mock') === '1';
  if (mock) {
    const bpm = TARGET_BPM, beatMs = 60000 / bpm;
    let t0 = performance.now();
    setInterval(() => {
      const t = performance.now() - t0;
      const phase = (t % beatMs) / beatMs;
      const kick = Math.exp(-phase * 8);
      ingest({
        t, bands: [0.4 * kick + 0.05, 0.15 + 0.1 * Math.sin(t / 900), 0.1 + 0.08 * Math.sin(t / 300), 0.05 + 0.05 * (phase > 0.5 ? 1 : 0)],
        peak: 0.6 * kick + 0.2, xf: 0,
        decks: [{ play: 1, bpm, beat: phase < 0.2 ? 1 : 0, bd: phase, vu: 0.7 }, { play: 0, bpm: 0, beat: 0, bd: 0, vu: 0 }]
      });
    }, 1000 / 30);
    return;
  }

  const source = new EventSource(FEED_URL);
  source.onmessage = (ev) => {
    try { ingest(JSON.parse(ev.data)); } catch (e) { console.warn('feed: bad frame', e); }
  };
  source.onerror = () => { /* EventSource reconnects on its own */ };
})();
