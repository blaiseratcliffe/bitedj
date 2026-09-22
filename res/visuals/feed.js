// The music signal. One EventSource on the engine's feed; smoothing, AGC and
// beat-edge detection live here so sketches read plain 0..1 numbers.
//
// Two beat-listener lists: onBeat(fn) is sketch-scoped and is wiped by
// clearBeatListeners() on every sketch switch (the director does this in
// show(), so a sketch never has to unsubscribe itself); onBeatAlways(fn) is
// permanent and is what the director's own rotation logic uses, so it keeps
// running across switches. Both lists fire on every beat edge.
//
// ?mock=1 replaces the engine with a 174 BPM synthetic feed for desktop work.
(function () {
  const FEED_URL = 'http://127.0.0.1:7374/events';
  const BANDS = ['bass', 'lowmid', 'mid', 'high'];
  const ATTACK = 0.6;    // fraction of the way to a louder value per frame
  const RELEASE = 0.12;  // fraction of the way to a quieter value per frame
  const AGC_DECAY = 0.995; // running max decays slowly, so quiet passages still move
  const AGC_FLOOR = 0.02;
  const DEAD_AFTER_MS = 2000;
  const BEAT_VISIBLE_FRAMES = 2; // rAF ticks feed.beat stays true after an edge

  const feed = {
    bass: 0, lowmid: 0, mid: 0, high: 0, peak: 0,
    bpm: 0, beat: false, beats: 0, playing: false, alive: false,
    _beatFns: [], _beatAlwaysFns: [], _beatFrames: 0,
    _lastFrameAt: 0, _max: [AGC_FLOOR, AGC_FLOOR, AGC_FLOOR, AGC_FLOOR],
    _prevBeat: [], _masterDeck: -1,
    onBeat(fn) { this._beatFns.push(fn); },
    onBeatAlways(fn) { this._beatAlwaysFns.push(fn); },
    clearBeatListeners() { this._beatFns.length = 0; }
  };
  window.feed = feed;
  window.a = {
    fft: [0, 0, 0, 0],
    setBins() {}, setSmooth() {}, setScale() {}, setCutoff() {}, show() {}, hide() {}
  };

  function smooth(prev, next) {
    return prev + (next > prev ? ATTACK : RELEASE) * (next - prev);
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

  function ingest(frame) {
    feed._lastFrameAt = performance.now();
    feed.alive = true;
    BANDS.forEach((name, i) => {
      const raw = frame.bands[i] || 0;
      feed._max[i] = Math.max(raw, feed._max[i] * AGC_DECAY, AGC_FLOOR);
      const norm = Math.min(1, raw / feed._max[i]);
      feed[name] = smooth(feed[name], norm);
      window.a.fft[i] = feed[name];
    });
    feed.peak = smooth(feed.peak, Math.min(1, frame.peak || 0));

    const decks = frame.decks || [];
    feed.playing = decks.some(d => d.play);
    feed._masterDeck = pickMaster(decks, frame.xf || 0);
    feed.bpm = feed._masterDeck >= 0 ? decks[feed._masterDeck].bpm : 0;

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
    if (beatNow) {
      feed.beat = true;
      feed._beatFrames = 0;
      feed.beats += 1;
      feed._beatFns.forEach(fn => fn());
      feed._beatAlwaysFns.forEach(fn => fn());
    }
  }

  // Clear the one-frame beat flag and the liveness flag on the render
  // clock, not the network clock. feed.beat stays true for
  // BEAT_VISIBLE_FRAMES rendered frames after an edge, then this clears it;
  // ingest() above never clears it itself.
  function housekeeping() {
    if (feed.beat) {
      feed._beatFrames += 1;
      if (feed._beatFrames >= BEAT_VISIBLE_FRAMES) feed.beat = false;
    }
    feed.alive = performance.now() - feed._lastFrameAt < DEAD_AFTER_MS;
    requestAnimationFrame(housekeeping);
  }
  requestAnimationFrame(housekeeping);

  const mock = new URLSearchParams(location.search).get('mock') === '1';
  if (mock) {
    const bpm = 174, beatMs = 60000 / bpm;
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
