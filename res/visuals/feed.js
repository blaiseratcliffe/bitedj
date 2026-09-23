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
//   feed.pulse   rises to about 0.85 over two or three rendered frames on a
//                beat edge and decays with a time constant of a third of a
//                beat. The fastest signal in the file, and the accents it
//                drives should be worth a few percent, not a slam.
//   feed.bounce  an underdamped spring kicked on every beat edge: it snaps
//                to about 1 in 40 ms, overshoots below zero and settles in
//                about 350 ms, a beat at 174 and half of one at 88. Read it
//                as a multiplier on the scale of a sketch's main element,
//                1 + 0.08 * feed.bounce, so the foreground pops on the kick
//                and springs back while the background keeps drifting on
//                the swell. This is the one signal here that is allowed to
//                overshoot, and it is what the smoothness rework took out
//                too completely: pulse is a brightness accent worth a few
//                percent, bounce is motion.
//   feed.phase   0..1 through the current beat, free running on the render
//                clock and corrected forward from the master deck's
//                beat_distance. Continuous rotation locked to tempo.
//   feed.spring(name, target, stiffness)
//                a critically damped spring per name, so a beat can retarget
//                something and the picture eases there instead of cutting.
//
// What energy and swell are measured against. The raw bands are normalised
// twice, and the two normalisations answer different questions:
//
//   - The raw bands (bass, lowmid, mid, high, peak) sit on a fast AGC, a
//     running maximum that halves every four and a half seconds. That is the
//     right thing for a twitchy per-kick number, and it is what a community
//     sketch reading a.fft expects. It is the WRONG thing for the envelopes,
//     and the first version fed them from it: dividing by the loudest thing
//     in the last few seconds measures crest factor, not loudness, so a quiet
//     steady break normalised to 1.0 and read LOUDER than the drop, and swell
//     travelled about 0.2 over a whole track, the wrong way (fork issue #4).
//   - The envelopes are normalised against a slow reference instead: the
//     running maximum of the one bar average, halving only every three
//     minutes. So swell is 1 through the loudest passage the room has heard
//     lately and sinks through a break, energy is the same scale with the
//     kicks riding on top, and a whole track sets the reference rather than
//     the last kick. The reference has a floor so a quiet intro at the start
//     of a set reads quiet rather than being stretched up to full scale
//     before the first drop has arrived.
//
// REACTIVITY scales all three envelopes before they are clamped, and it is
// the one knob for how hard the picture leans into the music: 0.6 is subtle,
// 1 is the tuned default, 1.5 is strong, where a drop pins swell at 1 for its
// whole length. ?react=1.5 on the URL overrides it for a session, the same
// way ?mock=1 does the feed.
//
// The tempo is the deck's, taken as it comes. A version of this file treated
// a master deck at 60 to 100 BPM as a half-time reading of a drum and bass
// grid and doubled it, with a synthetic beat edge at the midpoint of every
// grid beat. It was written against a deck showing 88.0, and the track on
// that deck turned out to be an 88 BPM tune: the guard would have pulsed on
// every offbeat of it. If the analyser ever does read a grid at half tempo,
// the fix belongs in the analyser, where the whole engine benefits, not here.
//
// The raw bands are still mirrored onto a.fft for community sketches, but
// they are the twitchiest thing in the file and sketches in this project
// should prefer the envelopes.
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
// The mock has an eight bar break in every thirty-two, so the swell has
// somewhere to go; the first mock did not, and every amount tuned against it
// was tuned against a swell that never moved.
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
  const PULSE_BEATS = 1 / 3;   // pulse decay time constant, in beats
  const PULSE_ATTACK_MS = 25;  // and how long it takes to get there
  // What the pulse peaks at, whatever the tempo. The edge sets the target
  // above this and the read value is clamped at 1, because the decay and the
  // attack race each other from the first frame: with the target at exactly
  // 1 the peak came out at 0.55 at 174 BPM and 0.70 at 88, not the 0.85 the
  // comments promised, so every accent was two thirds of its intended size
  // and a different two thirds at every tempo. pulseGain() below solves the
  // chase for the peak it would reach in continuous time and scales the
  // target to hit this; the render loop then samples either side of that
  // peak, and at 25 to 30 fps what a sketch reads tops out about a tenth
  // lower. 0.95 here is 0.84 at 174 BPM and 0.87 at 88 in the replay, which
  // is the "about 0.85" the header promises.
  const PULSE_PEAK = 0.95;
  // The bounce oscillator. The rise time sets the natural frequency and the
  // damping ratio sets how far it overshoots: at 0.35 the first undershoot
  // is about a third of the peak and the second overshoot a tenth, which
  // reads as one bounce and not a wobble. 40 ms is two rendered frames, the
  // same rule as the pulse's attack: a thing that lands inside one frame is
  // a cut, not a movement. Neither is tempo-aware on purpose. A kick is a
  // kick at any tempo, and at 174 the settle overlapping the next kick is
  // what makes a run of kicks read as a groove rather than a metronome.
  const BOUNCE_RISE_MS = 40;
  const BOUNCE_DAMPING = 0.35;
  const TARGET_BPM = 174;    // what the box is built for, and the bpm fallback
  const BARS_OF_SWELL = 4;
  const MAX_DT_MS = 100;     // a stall must not integrate as if it were real

  // The slow reference the envelopes are measured against: see the header.
  const REF_HALF_LIFE_MS = 180000;
  // Raw bass RMS. The kick on this box measured 0.21 with the master at
  // 0 dB and 0.07 to 0.11 between kicks (docs/VISUALS.md section 5), so a
  // floor at a fifth of the kick keeps a quiet intro reading quiet.
  const REF_FLOOR = 0.04;
  // How far above the bar average the 120 ms envelope peaks on a kick-driven
  // passage. It is what keeps energy from pinning at 1 through a whole drop:
  // the fast half of energy is measured against the reference times this, so
  // its ripple over each beat survives at the top of the scale. Measured off
  // a capture of a drum and bass drop on bitepi: the kick reaches 0.17 to
  // 0.21 RMS against a bar average of 0.10, and at 1.4 the replay clamped
  // energy at 1.00 on every kick; at 2 it peaks in the high 0.8s.
  const ENV_CREST = 2;

  const params = new URLSearchParams(location.search);
  const REACTIVITY = (() => {
    const v = parseFloat(params.get('react'));
    return v > 0 && v < 10 ? v : 1;
  })();

  const feed = {
    bass: 0, lowmid: 0, mid: 0, high: 0, peak: 0,
    // The bass band as the engine sent it, before any normalisation: RMS of
    // the master mix below 120 Hz over the last 33 ms window. For the log
    // line and the replay harness, not for sketches.
    rawBass: 0,
    bpm: 0, beat: false, beats: 0, playing: false, alive: false,
    energy: 0, swell: 0, pulse: 0, bounce: 0, phase: 0,
    reactivity: REACTIVITY,
    _beatFns: [], _beatAlwaysFns: [], _beatFrames: 0,
    _lastFrameAt: 0, _max: [AGC_FLOOR, AGC_FLOOR, AGC_FLOOR, AGC_FLOOR],
    _prevBeat: [], _masterDeck: -1,
    // Targets set by ingest() at 30 Hz, chased by the render clock.
    _want: { bass: 0, lowmid: 0, mid: 0, high: 0, peak: 0, energy: 0, swell: 0 },
    _env: 0, _bar: 0, _swell: 0, _ref: REF_FLOOR,
    _pulseWant: 0, _bounceV: 0, _springs: Object.create(null),
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

  function clamp01(v) {
    return v < 0 ? 0 : (v > 1 ? 1 : v);
  }

  // The target a beat edge sets the pulse to, so that the value a sketch
  // reads peaks at PULSE_PEAK. The pulse chases a target that decays with
  // time constant tau (a third of a beat) through an attack with time
  // constant a (PULSE_ATTACK_MS): x' = (W e^(-t/tau) - x) / a, whose solution
  // is x = W tau / (tau - a) (e^(-t/tau) - e^(-t/a)). That peaks at
  // t* = ln(tau / a) / (1/a - 1/tau), and W is chosen so the peak is
  // PULSE_PEAK. The render loop discretises the chase at 25 to 30 fps, which
  // lands the measured peak within a few hundredths of this.
  function pulseGain(beatMs) {
    const tau = PULSE_BEATS * beatMs, a = PULSE_ATTACK_MS;
    const tStar = Math.log(tau / a) / (1 / a - 1 / tau);
    const peak = tau / (tau - a) * (Math.exp(-tStar / tau) - Math.exp(-tStar / a));
    return PULSE_PEAK / peak;
  }

  // The bounce oscillator's constants, solved once from the rise time and
  // the damping ratio. x'' + 2 a x' + w^2 x = 0 with a = zeta w, whose
  // impulse response from rest is x = (v0 / wd) e^(-a t) sin(wd t), peaking
  // at t* = atan2(wd, a) / wd. w is chosen so t* is BOUNCE_RISE_MS and v0 so
  // the peak is exactly 1.
  const BOUNCE = (() => {
    const zeta = BOUNCE_DAMPING;
    const root = Math.sqrt(1 - zeta * zeta);
    const w = Math.atan2(root, zeta) / (BOUNCE_RISE_MS * root);
    const a = zeta * w, wd = w * root;
    const tStar = Math.atan2(wd, a) / wd;
    const peak = Math.exp(-a * tStar) * Math.sin(wd * tStar) / wd;
    return { a, wd, v0: 1 / peak };
  })();

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

    // The raw bands, on the fast AGC. These are what feed.bass and a.fft
    // carry, and nothing below reads them.
    let rawBass = 0;
    BANDS.forEach((name, i) => {
      const raw = frame.bands[i] || 0;
      feed._max[i] = Math.max(raw, feed._max[i] * AGC_DECAY, AGC_FLOOR);
      feed._want[name] = smooth(feed._want[name], Math.min(1, raw / feed._max[i]));
      if (i === 0) rawBass = raw;
    });
    feed.rawBass = rawBass;
    feed._want.peak = smooth(feed._want.peak, Math.min(1, frame.peak || 0));

    const decks = frame.decks || [];
    feed.playing = decks.some(d => d.play);
    const master = pickMaster(decks, frame.xf || 0);
    feed._masterDeck = master;
    feed.bpm = master >= 0 ? decks[master].bpm : 0;

    // The envelopes, on the raw bass and the slow reference. The energy
    // attack and release are in milliseconds rather than per-frame fractions
    // so they mean the same thing whatever the feed does, and the release is
    // six times the attack: the picture should arrive with the kick and
    // leave long after it. The bar average and the swell are exponential
    // running averages rather than ring buffers over the last N samples: same
    // mean, no discontinuity when a sample falls out of the window, and no
    // buffer to resize when the tempo changes under them.
    const tau = rawBass > feed._env ? ENERGY_ATTACK_MS : ENERGY_RELEASE_MS;
    feed._env += (rawBass - feed._env) * approach(dt, tau);
    const barMs = 4 * feed.beatMs();
    feed._bar += (rawBass - feed._bar) * approach(dt, barMs);
    feed._swell += (rawBass - feed._swell) * approach(dt, BARS_OF_SWELL * barMs);

    // The reference follows the bar average up at once and decays from it
    // with a three minute half-life. Written as a power of dt so a dropped
    // frame decays it by the right amount rather than by one frame's worth.
    feed._ref = Math.max(feed._bar, feed._ref * Math.pow(0.5, dt / REF_HALF_LIFE_MS), REF_FLOOR);

    const fast = feed._env / (feed._ref * ENV_CREST);
    const slow = feed._bar / feed._ref;
    feed._want.energy = clamp01(REACTIVITY * (0.5 * fast + 0.5 * slow));
    feed._want.swell = clamp01(REACTIVITY * feed._swell / feed._ref);

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
    const deck = master >= 0 ? decks[master] : null;
    if (deck && typeof deck.bd === 'number') {
      const bd = deck.bd - Math.floor(deck.bd);
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
      feed._pulseWant = pulseGain(feed.beatMs()) * REACTIVITY;
      // An impulse added to whatever motion is left from the last kick,
      // rather than a reset, so a fast run of kicks builds a groove and a
      // kick that lands on a settling bounce does not snap it to rest first.
      feed._bounceV += BOUNCE.v0 * REACTIVITY;
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
  let pulseRaw = 0;
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

    // The pulse is an envelope, not a step. The beat edge sets the target
    // above 1 (pulseGain) and the target decays from there with the
    // beat-third time constant; the raw value chases that target with a
    // 25 ms attack, so the rise takes two or three rendered frames instead of
    // landing inside one, and what a sketch reads is the raw value clamped
    // at 1. The soft rise matters wherever a pulse feeds something with a
    // hard edge of its own: the edge detector gains in sketches.js sit in
    // front of a fixed threshold, and a value that goes 0 to 1 between two
    // frames adds and drops whole edges at once.
    const beatMs = feed.beatMs();
    feed._pulseWant *= Math.exp(-dt / (PULSE_BEATS * beatMs));
    pulseRaw += (feed._pulseWant - pulseRaw) * approach(dt, PULSE_ATTACK_MS);
    if (pulseRaw < 1e-4 && feed._pulseWant < 1e-4) { pulseRaw = 0; feed._pulseWant = 0; }
    feed.pulse = clamp01(pulseRaw);

    // The bounce, advanced by the exact solution of the damped oscillator
    // over dt, for the same reason the springs below are: a 100 ms frame
    // must land on the curve, not fly off it. With A = x and
    // B = (v + a x) / wd, x(t) = e^(-a t)(A cos wd t + B sin wd t) and
    // v(t) is its derivative.
    {
      const { a, wd } = BOUNCE;
      const x = feed.bounce, v = feed._bounceV;
      const A = x, B = (v + a * x) / wd;
      const e = Math.exp(-a * dt), c = Math.cos(wd * dt), s = Math.sin(wd * dt);
      feed.bounce = e * (A * c + B * s);
      feed._bounceV = e * ((wd * B - a * A) * c - (wd * A + a * B) * s);
      if (Math.abs(feed.bounce) < 1e-4 && Math.abs(feed._bounceV) < 1e-5) {
        feed.bounce = 0; feed._bounceV = 0;
      }
    }

    feed.phase += dt / beatMs;
    if (feed.phase >= 1) feed.phase -= Math.floor(feed.phase);

    // Exact solution of a critically damped spring over dt, so the step is
    // stable at any frame rate and the spring cannot ring or explode when the
    // compositor hands us a 100 ms frame. for-in rather than Object.keys,
    // which would allocate an array on every rendered frame for the sake of
    // the two or three springs a sketch actually has.
    const secs = dt / 1000;
    for (const name in feed._springs) {
      const s = feed._springs[name];
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

  // Exposed for the replay harness (toolchain/visuals/replay-feed.js), which
  // drives the real ingest() from a recorded capture instead of a socket.
  feed._ingest = ingest;

  const mock = params.get('mock') === '1';
  if (mock) {
    const bpm = TARGET_BPM, beatMs = 60000 / bpm;
    let t0 = performance.now();
    setInterval(() => {
      const t = performance.now() - t0;
      const phase = (t % beatMs) / beatMs;
      const bar = Math.floor(t / (4 * beatMs)) % 32;
      const kick = bar < 24 ? Math.exp(-phase * 8) : 0;
      const pad = bar < 24 ? 0.05 : 0.03;
      ingest({
        t, bands: [0.16 * kick + pad, 0.15 + 0.1 * Math.sin(t / 900), 0.1 + 0.08 * Math.sin(t / 300), 0.05 + 0.05 * (phase > 0.5 ? 1 : 0)],
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
