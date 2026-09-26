// Runs the show: sources, sketch rotation, the fps cap, the idle fallback and
// the numbers the load test reads from ~/bitedj-visuals.log.
//
// Idle is a state, not a rotation. Twenty seconds without a beat means
// nothing is playing or the feed is down, and the idle sketch then stays up
// until the next beat arrives. Nobody is watching an idle screen, and
// swapping sketches on a timer only spent GPU on an empty room.
//
// Outputs. o0 is the sketch, o1 is the sketch's own scratch, and o2 and the
// source s3 are reserved by this file for the crossfade: nothing in
// sketches.js may write to either. o3 is not written by anything today and
// stays reserved here rather than being handed to the sketches, so the
// crossfade keeps a spare output. Sources s4 to s7 belong to patterns.js and
// a sketch reaches them through patterns.bind() rather than by hand. s8
// belongs to video.js: a sketch reads it with src(s8), and only video.js,
// through video.open() and video.close() called from here, ever inits or
// clears it. A switch
// used to be a 125 ms dip to black through a CSS opacity transition, which
// read as a cut with a hole in it. Now the outgoing frame is frozen, the new
// sketch starts straight away in o0, and o2 shows a mix of the two for two
// seconds while the frozen frame is pulled apart by a slow noise. Because the
// frozen frame is a still, the modulate is what makes it melt rather than
// fade. When the mix reaches the new sketch the display goes back to o0 and
// o2 is dropped to a blank chain, so the steady state pays for nothing but
// the sketch.
//
// The still is a copy of the canvas taken in afterUpdate and pushed into s3
// as a static source, not an output frozen on itself. The output version was
// written first and it came out as a two second fade up from black, because
// src(oN) samples the output's *previous* frame (Output.getTexture returns
// the fbo that is not the one just written) and the four outputs are drawn in
// order 0 to 3 within a tick. Freezing o3 with src(o3) therefore copies
// whatever o3 held a frame earlier, and o2, which is drawn before o3, reads
// o3 a frame later still. Getting a still out of that is a question of which
// fbo of a ping-pong pair is current at which point of a tick, which is not
// a thing to build a crossfade on. afterUpdate runs immediately after the
// frame is drawn and before the compositor clears the drawing buffer, which
// is the one moment the canvas can be read, so the copy there is exact and
// needs no reasoning about ping-pong at all. It also snapshots whatever is on
// screen, so a switch during a melt freezes the mix with no special case.
//
// The webcam is opened on demand rather than at load: s0.initCam(0) runs when
// a sketch that reads it is about to start and s0.clear() when the rotation
// reaches one that does not. Two kinds of sketch read it. `cam: true` is a
// camera sketch, which is the picture and drops out of the rotation when
// there is no camera; `camMix` names a treatment (bend, cut or edges) that
// mixes the camera into a sketch that stands on its own, and such a sketch
// stays in the rotation without a camera and simply runs plain. Every
// non-camera sketch carries a camMix today, so with a camera present it is
// open for the whole show, unless the Camera row's Mix switch is off, in
// which case show() never asks for it for those sketches and the release
// path below is what closes the camera when the rotation reaches one; the
// same release path is also what a library with a sketch that carries
// neither flag would use. hush() is still never called anywhere, for the
// reason by clearScratch().
//
// Video on s8 follows the same lifecycle. `video: true` is a video sketch,
// which is the footage and drops out of the rotation when no clip is
// available; `vidMix` names a treatment that mixes the footage into a sketch
// that stands on its own, and such a sketch runs plain without video. show()
// opens the video for either kind before the switch lands, and it is closed
// when a switch lands on a sketch that reads neither, so the decoder only
// runs while footage is on screen. The close waits for the landing rather
// than happening in show(), because the melt's still is taken from the frame
// after show() and a clip closed before it would melt out of a blank.
//
// Sketches drive their own per-frame work through window.sketchUpdate(dt),
// which this file calls every rendered frame; a sketch must never assign
// window.update or window.afterUpdate itself, both of those belong to this
// file (see the notes by the assignments below for why).
(function () {
  // 960x540, shown at 1920x1080 on the TV. 1920x1080 was tried on bitepi on
  // 2026-09-22 with one deck playing: the page fell from 24 fps to 8 to 15,
  // and the panel's waveform dropped to 34 to 38 fps with a dropped-frame
  // warning every ten seconds. The VideoCore cannot carry both at that size.
  // 1280x720 the same evening: the page ran 14 to 23 fps, mostly 17 to 21,
  // and the waveform held 60 fps but dropped 5 to 9 frames every ten
  // seconds where 960x540 drops none. Usable, at a cost to the instrument.
  const RENDER_W = 960, RENDER_H = 540;
  const FPS = 30;
  // From the Switch every row: 8, 16, 32 or 64 bars, read on every beat. The
  // count runs from the last switch, whatever caused it, so a sketch reached
  // by Next or by a settings melt gets a full interval too; the switches
  // were never aligned to phrases (fork issue #3), so nothing is lost by
  // resetting the count. The floor below is what stops a fast tempo
  // strobing through the library; 8 bars at 174 is 11 s, so the floor sits
  // under that.
  const beatsPerSwitch = () => 4 * (feed.settings.bars || 16);
  const MIN_SKETCH_MS = 8000;
  // How many sketches the picker keeps out of the next draw. With a switch
  // every 22 s and a colour family of seven, remembering only the last one
  // brought plasma-kaleid back seven times in an evening of twenty-two
  // switches; eight of twenty-four means everything gets a turn before
  // anything repeats, and the family preference still applies within what
  // is left.
  const HISTORY_LENGTH = 8;
  const IDLE_AFTER_MS = 20000;      // no beat for this long: idle sketch
  const MELT_MS = 2000;             // length of the crossfade
  const LOG_EVERY_MS = 10000;

  const canvas = document.getElementById('stage');
  // numSources 9 rather than the default 4. s0 to s3 are the webcam, the
  // wordmark, the boot logo and the melt's frozen frame; s4 to s7 are the two
  // pattern slots, two sources each because a pattern sketch always draws a
  // blend of two neighbouring sweep frames; s8 is the video clip, owned by
  // video.js. makeGlobal defines a window.sN for each one (EvalSandbox's
  // constructor walks Object.keys(synth), and _initSources has already run by
  // then), which is why patterns.js can reach s4 and video.js s8 without this
  // file handing them anything.
  const hydra = new Hydra({ canvas, width: RENDER_W, height: RENDER_H, detectAudio: false, makeGlobal: true, numSources: 9 });
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

  // Sources. s0 the webcam, opened by show() when a cam sketch needs it;
  // s1 the wordmark, s2 the boot logo.
  window.camReady = false;

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
  let current = null, currentSince = 0, currentSinceBeat = 0, lastBeatAt = performance.now(), idle = false;
  let history = [];
  // Whether s0 currently holds an open camera stream. Only show() moves this.
  let camInit = false;
  // Preview mode's two hooks, set only by preview.js and only under
  // ?preview=1 (see window.director at the bottom). `paused` stops the beat
  // rotation and nothing else; `switchHook` is told about every landed switch.
  // Outside preview mode they stay false and null for the life of the page.
  let paused = false, switchHook = null;
  // Preview mode's camera, set only by preview.js through useCamera(). A page
  // served over http has no camera at all (browsers only allow one on https
  // or localhost), so the preview feeds s0 from a recorded stand-in clip or
  // a test pattern instead, and says which. Null on the Pi for the life of
  // the page.
  let camImpl = null;
  function camOpen() { if (camImpl) camImpl.open(); else s0.initCam(0); }
  function camClose() { if (camImpl) camImpl.close(); else s0.clear(); }

  // Sequences (visuals library Part B). A Sequence set is a running order
  // that sets.js walks; the director asks it for the next entry wherever a
  // Random set would call pickNext(), and remembers the entry on screen for
  // its length and its log line. `entryPending` and `entryQueued` travel with
  // `pending` and `queued` through show(), so an entry held behind a melt is
  // still an entry when it lands.
  let entryNow = null, entryPending = null, entryQueued = null;
  // The entry whose clip show() already opened, so startPending() does not
  // open it a second time.
  let clipOpenedFor = null;
  // feed.beats when a due switch began waiting for a pattern, or -1.
  let holdSince = -1;
  const HOLD_BEATS = 32;        // 8 bars
  // Whether the hold under holdSince is a resume's, waiting on the first beat
  // out of idle for the replayed entry's pattern (resumeEntry()).
  let resuming = false;
  // A switch into a sequence wants to happen on the next beat, not at the
  // end of whatever the previous set left on screen.
  let switchWanted = false;
  let wasSequence = false;
  // Whether "has nothing that can play" has been said since the last entry
  // landed; the rotation asks again every interval and once is enough.
  let nothingNoted = false;

  const inSequence = () => !!window.sets && sets.isSequence();
  const sketchNamed = (name) => window.sketches.find(s => s.name === name) || null;

  // Whether an entry can play now. The pattern test takes the rotation's own
  // exclusions (over size, retired, failed to load, skipped this session)
  // but not "too solid", which is a verdict on the look: an entry that names
  // the pattern has made its own.
  const PLAYABLE_PATTERN = ['in rotation', 'too solid'];
  function canPlayEntry(e) {
    const s = sketchNamed(e.sketch);
    if (!s) return false;
    if (s.cam && !(window.camReady && feed.settings.camSketches === 1)) return false;
    if (s.pattern) {
      if (!window.patterns || feed.settings.patterns !== 1 || !e.pattern) return false;
      const p = patterns.library().find(x => x.slug === e.pattern);
      if (!p || PLAYABLE_PATTERN.indexOf(p.status) < 0) return false;
    }
    if (s.video) {
      if (!window.video || !e.clip) return false;
      if (!video.clips().some(c => c.file === e.clip)) return false;
      if (video.failed().indexOf(e.clip) >= 0) return false;
    }
    return true;
  }

  function patternReady(slug) {
    if (!window.patterns) return false;
    if (patterns.pinLoading() === slug) return false;
    return patterns.library().some(p => p.slug === slug && p.cached);
  }

  // Starts loading the pattern of the next entry, so it is in the cache by
  // the time that entry is due. Called after each landing, and at a switch
  // into a sequence for entry 1.
  function prefetch() {
    if (!inSequence() || !window.patterns) return;
    const up = sets.peek(canPlayEntry);
    if (up && up.pattern && patterns.pinned() !== up.pattern) {
      patterns.pin(up.pattern, (entry) => {
        if (!entry) console.log('visuals: pattern ' + up.pattern + ' could not be loaded for the sequence');
      });
    }
  }

  function skipLine(e, why) {
    console.log('visuals: sets ' + sets.active().id + ' entry ' + e.index + '/' + e.total + ' '
      + e.sketch + ' skipped, pattern ' + e.pattern + ' not loaded' + why);
  }

  function showEntry(e) {
    if (!e) {
      // Once is enough: the rotation keeps asking every switch interval, and
      // melting the idle sketch into itself each time would be a visible pulse.
      if (!nothingNoted) console.log('visuals: sets ' + sets.active().id + ' has nothing that can play');
      nothingNoted = true;
      if ((queued || pending || current) !== window.idleSketch) show(window.idleSketch);
      return;
    }
    // The pattern is pinned in startPending(), at the landing; see there.
    show(sketchNamed(e.sketch), e);
  }

  // The show's next move. Outside a sequence, the random pick. In one, the
  // next entry: a due rotation (manual false) holds for a pattern that is
  // not in the cache yet, for up to HOLD_BEATS; anything else (Next, a lost
  // camera or clip, a setting that took the sketch away) skips it now.
  function advance(manual) {
    if (!inSequence()) { show(pickNext()); return; }
    // A resume still waiting for its pattern answers first: the entry it is
    // waiting for is the one this move is about.
    if (resuming) { resumeEntry(manual); return; }
    const any = sets.peek(() => true);
    const total = any ? any.total : 0;
    for (let tries = 0; tries <= total; tries++) {
      const e = sets.peek(canPlayEntry);
      if (!e || !e.pattern || patternReady(e.pattern)) {
        holdSince = -1;
        showEntry(sets.next(canPlayEntry));
        return;
      }
      if (!manual) {
        if (holdSince < 0) {
          holdSince = feed.beats;
          if (patterns.pinned() !== e.pattern) patterns.pin(e.pattern);
          console.log('visuals: holding for pattern', e.pattern);
          return;
        }
        if (feed.beats - holdSince < HOLD_BEATS) return;
      }
      sets.next(canPlayEntry);                // consume the entry being skipped
      skipLine(e, manual ? '' : ' after 8 bars');
      holdSince = -1;
      manual = true;                          // one hold per switch, no more
    }
    // Every entry in a full pass was an unloaded pattern: keep what is on
    // screen and try again on the next beat.
    console.log('visuals: sets ' + sets.active().id + ' has no entry ready; holding');
  }

  // Leaving idle in a sequence: the entry that idle interrupted, again, or
  // entry 1 after a switch made while idle. Its pattern can still be loading:
  // a sequence picked before the music starts pins entry 1's pattern at the
  // switch, and the first beat can come before the load lands. So a resume
  // gets the hold a due rotation gets. The idle sketch stays up, every beat
  // asks again (the rotation calls advance() while holdSince is set, and
  // advance() comes back here while `resuming`), and after HOLD_BEATS the
  // entry is skipped with the skip line; a manual move skips it at once, also
  // with the line. Asking again is safe because replay() answers the same
  // entry on every call until something else is returned. Never silent: an
  // entry lost here used to be lost with no line, and an intro for good.
  function resumeEntry(manual) {
    const e = sets.replay(canPlayEntry);
    if (!e || !e.pattern || patternReady(e.pattern)) {
      resuming = false; holdSince = -1;
      showEntry(e);
      return;
    }
    if (!manual) {
      // holdSince < 0 also when a landing reset it, which is the idle sketch
      // melting in again after another 20 s without a beat: the hold starts over.
      if (!resuming || holdSince < 0) {
        resuming = true;
        holdSince = feed.beats;
        if (patterns.pinned() !== e.pattern) patterns.pin(e.pattern);
        console.log('visuals: holding for pattern', e.pattern);
        return;
      }
      if (feed.beats - holdSince < HOLD_BEATS) return;
    }
    resuming = false; holdSince = -1;
    skipLine(e, manual ? '' : ' after 8 bars');
    advance(true);
  }

  // The idle sketch has landed in a sequence. The first beat back replays
  // the interrupted entry, so its pattern, not the next entry's, is the one
  // to keep: pinned, the cache cannot rotate it out during a long idle.
  // replay() is exactly what the resume will ask, and it answers the same
  // entry until something else is returned, so asking it now changes nothing.
  function keepForResume() {
    if (!window.patterns) return;
    const e = sets.replay(canPlayEntry);
    if (e && e.pattern && patterns.pinned() !== e.pattern) patterns.pin(e.pattern);
  }

  const beatsForCurrent = () =>
    (entryNow && inSequence() ? 4 * (entryNow.bars || feed.settings.bars || 16) : beatsPerSwitch());

  // Crossfade. `melt` is 1 the moment a new sketch starts and eases to 0 over
  // MELT_MS; `pending` holds a sketch whose snapshot frame has been armed but
  // not yet captured, and `queued` a request that arrived while all that was
  // in flight. `nextPending` is separate: it counts Next taps that arrive
  // while busy(), up to three, and endMelt() drains one into its own show()
  // each time a melt ends, so three taps in a row still give three melts.
  //
  // `grabbing` is set by show() and cleared by the afterUpdate hook that
  // takes the still; `swapNext` is then read by the driveFrame after it, so
  // the new sketch starts on the first tick that has a still to melt out of.
  let melt = 0, meltElapsed = 0, melting = false, pending = null, queued = null;
  let grabbing = false, swapNext = false;
  let nextPending = 0;

  // The frozen frame. 960x540 like the render target, so the copy is one to
  // one and s3 needs no aspect correction.
  const still = document.createElement('canvas');
  still.width = RENDER_W; still.height = RENDER_H;
  const stillCtx = still.getContext('2d');
  // Bind the canvas to s3 once, here, and never again: HydraSource.init
  // assigns a fresh regl.texture and drops the old one on the floor without
  // destroying it, and regl keeps every texture it created registered, so a
  // re-init per switch leaks about two megabytes each time. At 256 beats a
  // switch that is roughly 85 MB an hour, which a box that runs a whole set
  // does notice. Every later grab writes into this texture with subimage()
  // instead; the canvas never changes size, so the shape always matches.
  s3.init({ src: still, dynamic: false });

  // hydra resolves the camera asynchronously and swallows the rejection, so
  // probe the device list ourselves to know whether camera sketches are
  // usable at all. Re-run on devicechange: a USB webcam can be plugged in
  // hours into a set, and can equally be pulled out mid-sketch.
  // Every outcome is logged, including a probe that never settles: on the
  // appliance the first build logged nothing at all here, which left no way
  // to tell "no camera" from "enumerateDevices() hung".
  const PROBE_TIMEOUT_MS = 5000;
  function probeCamera() {
    if (camImpl) return Promise.resolve();
    if (!navigator.mediaDevices || !navigator.mediaDevices.enumerateDevices) {
      window.camReady = false;
      console.error('visuals: camera probe unavailable: navigator.mediaDevices is',
        String(navigator.mediaDevices));
      return Promise.resolve();
    }
    const timeout = new Promise((_, reject) =>
      setTimeout(() => reject(new Error('timed out after ' + PROBE_TIMEOUT_MS + ' ms')), PROBE_TIMEOUT_MS));
    return Promise.race([navigator.mediaDevices.enumerateDevices(), timeout]).then(devs => {
      if (camImpl) return;
      const cams = devs.filter(d => d.kind === 'videoinput');
      window.camReady = cams.length > 0;
      console.log('visuals: camera', window.camReady ? 'present' : 'absent',
        '(' + devs.length + ' media devices, ' + cams.length + ' video inputs)');
      if (!window.camReady && current && (current.cam || current.camMix)) {
        // The camera went away underneath a sketch that is drawing it. Waiting
        // for the next beat switch could mean a minute of a frozen last frame,
        // so move on now; pickNext() already excludes cam sketches while
        // camReady is false, a camMix sketch picked next runs plain, and
        // show() releases s0 on the way out.
        console.log('visuals: camera lost during', current.name);
        advance(true);
      }
    }).catch(e => {
      if (camImpl) return;
      window.camReady = false;
      console.error('visuals: camera probe failed:', e && e.message ? e.message : String(e));
    });
  }
  probeCamera();
  if (navigator.mediaDevices && navigator.mediaDevices.addEventListener) {
    navigator.mediaDevices.addEventListener('devicechange', probeCamera);
  }

  // The three switches that change what a sketch is built from, and the
  // Next counter, watched on every rendered frame. A change that affects
  // the sketch on screen melts to another one now; one that does not waits
  // for the rotation. A Next tap always takes the show out of idle at once,
  // even when the switch itself has to wait: taps during a melt are
  // counted, up to three, and each runs as its own melt when the previous
  // one ends.
  //
  // watched.next stays null, and next is never treated as tapped, until
  // feed.alive: a frame has to have been ingested for feed.settings.next to
  // carry a real value, and taking the baseline any earlier would let a
  // count left over from taps earlier in the app's session melt the show on
  // its own the moment that first frame lands. Once alive, the baseline is
  // kept current every frame, so a drop in next (an app restart resets it
  // to 0) simply becomes the new baseline rather than reading as a tap.
  //
  // The knobs in force are logged here too, not at page load: a load-time
  // log always prints SETTING_DEFAULTS, because the EventSource has not
  // delivered a frame yet, whatever mixxx.cfg actually holds. So
  // `console.log('visuals: settings', ...)` fires once, on the same first
  // call where feed.alive is true that takes the next baseline above, and
  // again whenever JSON.stringify(s) differs from the last line logged;
  // lastSettingsJson holds that line. A tap and a melt still log themselves
  // below.
  const watched = { camMix: null, camSketches: null, patterns: null, next: null };
  let lastSettingsJson = null;
  function watchSettings() {
    const s = feed.settings;
    const first = watched.next === null;
    // camMix only matters to a sketch that is actually reading the camera;
    // with no camera camOn() is false either way, so a camMix change melts
    // nothing and window.camReady guards the clause against that no-op.
    const affected =
      (watched.camMix !== s.camMix && current && current.camMix && window.camReady) ||
      (watched.camSketches !== s.camSketches && current && current.cam && s.camSketches !== 1) ||
      (watched.patterns !== s.patterns && current && current.pattern && s.patterns !== 1);
    const tapped = watched.next !== null && s.next > watched.next;
    watched.camMix = s.camMix; watched.camSketches = s.camSketches;
    watched.patterns = s.patterns;
    if (feed.alive) watched.next = s.next;
    if (feed.alive) {
      const json = JSON.stringify(s);
      if (json !== lastSettingsJson) {
        console.log('visuals: settings', json);
        lastSettingsJson = json;
      }
    }
    // The active visuals set. sets.update() rereads the sets file only when
    // settings.set or settings.setRev moves, so calling it every frame costs
    // a comparison. Not before feed.alive, for the same reason as the Next
    // baseline above: until a frame arrives, set is the page's default 0.
    if (feed.alive && window.sets) sets.update(s);
    if (first) return;
    if (tapped) {
      console.log('visuals: next tapped');
      // A tap always leaves idle right away, whether or not the switch
      // itself can happen this instant: with no beat following, the 1 s
      // interval below drops back to the idle sketch after IDLE_AFTER_MS
      // as usual, rather than the idle sketch just sitting there stale.
      idle = false; lastBeatAt = performance.now();
      if (busy()) { nextPending = Math.min(nextPending + 1, 3); } else { advance(true); }
      return;
    }
    if (affected && !idle) { console.log('visuals: settings changed under', current.name); advance(true); }
  }

  // window.update, not hydra.synth.update: same makeGlobal mirroring as
  // fps above (EvalSandbox.tick() copies window.update onto synth.update
  // every frame), so this is the assignment that has to stick. It both
  // counts frames for the fps log, carries out an armed switch, advances the
  // crossfade and drives whatever the current sketch has put on
  // window.sketchUpdate. It runs before the outputs are drawn for the frame,
  // which is what makes the two-tick freeze above work.
  let frames = 0;
  function driveFrame(dt) {
    frames += 1;
    sampleRange();
    watchSettings();
    if (swapNext) {
      swapNext = false;
      startPending();
    } else if (melting) {
      meltElapsed += dt;
      const u = Math.min(1, meltElapsed / MELT_MS);
      // Ease in and out, so the mix neither starts nor ends with a visible
      // rate change. Linear here reads as a wipe that stops dead.
      melt = 1 - u * u * (3 - 2 * u);
      if (u >= 1) endMelt();
    }
    if (typeof window.sketchUpdate === 'function') {
      try { window.sketchUpdate(dt); } catch (e) { console.error('visuals: sketchUpdate failed', e); }
    }
  }

  // hush() (vendor/hydra-synth.js:3054-3065) also calls source.clear() on
  // every source (s0-s3): that stops the webcam's tracks and replaces the
  // wordmark/boot-logo textures (s1/s2) with 1x1 blanks, so every camera or
  // logo sketch would come up blank after the very first switch. This does
  // the safe subset: it drops the outgoing sketch's scratch chain, so a
  // sketch that never touches o1 does not keep paying for the noise field the
  // last one left running there. o0 is not cleared because the incoming
  // sketch overwrites its chain in the same tick, and o2 and o3 belong to the
  // crossfade.
  function clearScratch() {
    solid(0, 0, 0, 0).out(o1);
  }

  // A pattern sketch with nothing in the pattern cache is exactly a camera
  // sketch with no camera: it would come up on the wordmark fallback, which is
  // not what it is for. patterns.ready() goes true a few seconds after the
  // page loads and stays true, so this only ever excludes them at start-up or
  // when assets/patterns/ is missing altogether.
  // A video sketch is the same again, on video.available(): a clip exists and
  // not every clip has failed. It is not gated on video.ready(), which is
  // false whenever no video sketch is on screen, because show() opens the
  // clip as the sketch comes up.
  const videoOk = () => !!window.video && video.available();
  // A sketch that reads the video, either kind, while there is a clip to
  // play. A vidMix sketch with no clip runs plain and asks for nothing.
  const wantsVideo = (s) => !!(s.video || s.vidMix) && videoOk();
  // The last clip failing under a sketch that is drawing it is the camera
  // disappearing mid-sketch again: move on now rather than leaving a blank
  // source up until the next switch. pickNext() no longer offers a video
  // sketch, and a vidMix sketch picked next runs plain.
  if (window.video) {
    // The sketch the show is heading to, not only the one on screen: the
    // last clip can fail while a switch to a video sketch is still landing.
    video.onGiveUp(() => {
      const s = queued || pending || current;
      if (s && (s.video || s.vidMix)) {
        console.log('visuals: video lost during', s.name);
        advance(true);
      }
    });
  }
  // A Random set narrows the pool to what it ticks; Everything, an unreadable
  // sets file and a page without sets.js allow every sketch.
  const setAllows = (s) => !window.sets || sets.allows('sketch', s.name);
  function eligible() {
    return window.sketches.filter(s =>
      setAllows(s) &&
      (!s.cam || (window.camReady && feed.settings.camSketches === 1)) &&
      (!s.pattern || (window.patterns && patterns.ready() && feed.settings.patterns === 1)) &&
      (!s.video || videoOk()));
  }

  function pickNext() {
    const list = eligible();
    if (!list.length) return window.idleSketch;
    const last = history[history.length - 1];
    // Everything shown recently is out, as long as that leaves something;
    // on a box with no camera and no patterns the pool is thirteen and the
    // memory still leaves five. Only the last one is out unconditionally.
    let candidates = list.filter(s => !history.includes(s));
    if (!candidates.length) candidates = list.filter(s => s !== last);
    if (last) {
      // Step 1: prefer a sketch from the other family (colour vs monochrome).
      const otherFamily = candidates.filter(s => s.mono !== last.mono);
      if (otherFamily.length) candidates = otherFamily;
      // Step 2: avoid two logotype sketches back to back, when an alternative exists.
      if (last.name.startsWith('logo')) {
        const notLogo = candidates.filter(s => !s.name.startsWith('logo'));
        if (notLogo.length) candidates = notLogo;
      }
      // Step 3: the same for the pattern family, which is eight of the
      // twenty-four and would otherwise clump. Two pattern sketches in a row
      // are also the one pair that can share a cache entry, so the second
      // would often be the same artwork with a different treatment.
      if (last.pattern) {
        const notPattern = candidates.filter(s => !s.pattern);
        if (notPattern.length) candidates = notPattern;
      }
    }
    if (!candidates.length) candidates = list;
    return candidates[Math.floor(Math.random() * candidates.length)];
  }

  // A switch is in flight from the moment show() arms the snapshot until the
  // mix lands, about two and a tenth seconds later.
  function busy() { return melting || grabbing || swapNext; }

  function show(sketch, entry = null) {
    if (!sketch) { console.log('visuals: no sketch to show'); return; }
    // Nothing may start a crossfade on top of one that is already running.
    // The freeze, the swap and the mix are one sequence, and restarting it
    // from its own midpoint on every tick is how a melt turns into a stutter;
    // the rotation checks this for itself and simply waits a beat, but the
    // two paths that come in from outside it, the feed returning from idle
    // and a camera disappearing, are exactly the ones that never retry. So
    // the request is held rather than dropped and endMelt() runs it.
    if (busy()) { queued = sketch; entryQueued = entry; return; }
    // Camera lifecycle, decided here because this is the only place that sees
    // the switch itself. Done before the switch lands so the stream has the
    // crossfade to come up in. Both tests are the camInit flag against the
    // incoming sketch, deliberately not `current.cam`: `current` only updates
    // once the switch lands below, so show(camSketch) followed a frame later
    // by show(nonCamSketch), which is exactly what the devicechange path
    // does, would still see the previous non-cam `current`, skip the release,
    // and leave camInit stuck true with the stream open. No later cam sketch
    // could re-init after that.
    // A camMix sketch only asks for the camera when there is one to open and
    // the Camera row's Mix switch is on; a cam sketch cannot be here without
    // one, since eligible() drops it.
    const wantsCam = sketch.cam || (sketch.camMix && window.camReady && feed.settings.camMix === 1);
    if (wantsCam && !camInit) {
      try {
        camOpen();
        camInit = true;
        console.log('visuals: camera opened for', sketch.name);
      } catch (e) {
        console.error('visuals: initCam failed', e);
      }
    } else if (!wantsCam && camInit) {
      // s0.clear() stops the stream's tracks and leaves a 1x1 blank behind.
      // It is safe here, and only here, because no cam sketch is about to
      // draw s0; hush() would do this to s1 and s2 as well.
      try { camClose(); } catch (e) { console.error('visuals: camera release failed', e); }
      camInit = false;
      console.log('visuals: camera released');
    }
    // The video's half of the same decision. Opened here so the clip has the
    // crossfade to come up in; closed in startPending(), once the still has
    // been taken, for the reason in the header.
    // A sequence's video entry names its clip and plays it from the start.
    // With nothing open it opens here, so the clip has the melt to come up
    // in; with a clip already on screen it opens in startPending(), once the
    // still has been taken (see the header), because changing s8 now would
    // put a blank or the new clip's first frame into the still.
    if (entry && entry.clip && window.video) {
      if (!video.isOpen()) { video.open(entry.clip); clipOpenedFor = entry; }
    } else if (wantsVideo(sketch) && !video.isOpen()) {
      video.open();
    }
    // Arm the snapshot. The next rendered frame is copied into s3 by
    // grabStill() below and the switch happens on the frame after that.
    // Asking for a switch mid-melt snapshots the mix itself, so the picture
    // the new sketch melts out of is the one the eye was already on.
    pending = sketch;
    entryPending = entry;
    grabbing = true;
  }

  // window.afterUpdate, like window.update above, is mirrored onto the synth
  // every frame by the sandbox, and hydra calls it immediately after the
  // frame has been drawn. That is the only moment the canvas can be read: the
  // drawing buffer is not preserved, so a copy taken from a timer or a plain
  // rAF callback comes back empty, which is what a black crossfade looks
  // like. Nothing else in the page may take this property.
  function grabStill() {
    if (!grabbing) return;
    grabbing = false;
    try {
      // Black first, and not clearRect. The stage's drawing buffer carries
      // alpha and several sketches leave alpha 0 where they are black:
      // anything ending in edges() takes its alpha from luma(), so contours,
      // cam-contours, flow-lines and the idle sketch are transparent over
      // most of the frame. drawImage composites source-over, so without this
      // fill the still keeps whatever an earlier still left in those pixels,
      // for as long as the page runs. Black is what the screen showed there,
      // because the page behind the canvas is black.
      stillCtx.fillStyle = '#000';
      stillCtx.fillRect(0, 0, still.width, still.height);
      stillCtx.drawImage(canvas, 0, 0, still.width, still.height);
      // subimage, not init: see the note by the s3.init above. This is the
      // path the vendored regl takes, since its texture objects do carry a
      // subimage(). The fallback exists because a different build might not,
      // and there the texture has to be destroyed by hand first, because
      // HydraSource.init will not do it. s3 stays dynamic false either way,
      // so hydra never re-uploads it on its own and the still holds until the
      // next switch rather than tracking the new sketch.
      if (typeof s3.tex.subimage === 'function') {
        s3.tex.subimage(still);
      } else {
        try { s3.tex.destroy(); } catch (e2) { /* already gone */ }
        s3.init({ src: still, dynamic: false });
      }
      swapNext = true;
    } catch (e) {
      console.error('visuals: snapshot failed', e);
      // Without a still there is nothing to melt, but the switch still has to
      // happen, so let startPending run and the mix will simply come up on
      // whatever s3 last held.
      swapNext = true;
    }
  }
  window.afterUpdate = grabStill;

  // The second half of show(), one rendered frame later, with the outgoing
  // frame now sitting in s3.
  function startPending() {
    const sketch = pending;
    pending = null;
    const entry = entryPending; entryPending = null;
    if (!sketch) return;
    // A set change can land between show() and here. A sketch the set in
    // force no longer allows is dropped rather than melted in, and the show
    // arms the next one at once: the handler's pick if it queued one, else a
    // fresh pick. Nothing is melting yet, so show() arms rather than queues,
    // and the still it grabs is the frame still on screen. Without this a
    // video sketch under a set with no clip landed on a closed, blank s8.
    if (sketch !== window.idleSketch && !setAllows(sketch)) {
      // A queued request keeps its entry; with nothing queued, the show's next
      // move, which in a sequence is the next entry rather than a random pick.
      const next = queued, nextEntry = entryQueued;
      queued = null; entryQueued = null;
      console.log('visuals: sets ' + sets.active().id + ' dropped ' + sketch.name + ' before it landed');
      // advance(false), not true: nothing has landed, so this is the due
      // switch, and a switch into a sequence whose entry 1 pattern is still
      // loading holds for it with the old sketch on screen, as the rotation
      // would, rather than skipping entry 1 on the spot.
      if (next) show(next, nextEntry); else advance(false);
      return;
    }
    clearScratch();
    window.update = driveFrame;
    // Re-asserted for the same reason as window.update: a stray assignment
    // from anywhere else would stop the snapshot being taken and every later
    // switch would melt out of a stale still, silently.
    window.afterUpdate = grabStill;
    feed.clearBeatListeners();
    window.sketchUpdate = null;
    // Drop the outgoing sketch's claim on its patterns, so the cache can
    // rotate. The textures it uploaded stay bound until something rebinds
    // them; this only releases the canvases behind them.
    // A sequence entry's pattern is pinned here, at the landing, and not when
    // the entry was asked for: a request held behind a melt must not move the
    // pin under a pattern entry that is armed and about to run. take() in the
    // sketch's run() below hands this pattern out. Pinned before release(),
    // so the sweep in release() cannot evict it. Only while a sequence is in
    // force: the change handler strips the entry from a switch still landing
    // when the set changes, and this test is the second line, because a pin
    // left behind under a Random set would hand that pattern to every
    // pattern sketch for the rest of the session.
    if (entry && entry.pattern && window.patterns && inSequence()) patterns.pin(entry.pattern);
    if (window.patterns) patterns.release();
    // The still of the outgoing frame is in s3 by now, so nothing on screen
    // reads s8 any more if the incoming sketch does not.
    if (window.video && !wantsVideo(sketch) && video.isOpen()) video.close();
    // A named clip that show() left for now, because another was on screen.
    if (entry && entry.clip && window.video && clipOpenedFor !== entry) {
      video.open(entry.clip);
      clipOpenedFor = entry;
    }
    try {
      sketch.run();
    } catch (e) {
      console.error('visuals: sketch failed', sketch && sketch.name, e);
    }
    melt = 1; meltElapsed = 0; melting = true;
    src(o0)
      .blend(src(s3).modulate(noise(1.5, 0.08), () => 0.05 * melt), () => melt)
      .out(o2);
    render(o2);
    current = sketch; currentSince = performance.now(); currentSinceBeat = feed.beats;
    history.push(sketch); if (history.length > HISTORY_LENGTH) history.shift();
    // The set the switch happened under, then the sketch. Nothing else in
    // the page logs a line starting 'visuals: set <id> ' (sets.js and the
    // change handler below say 'visuals: sets '), so grepping for
    // 'visuals: set 2 ' gives every switch under set 2 and nothing more.
    entryNow = entry;
    const inForce = window.sets ? sets.active() : { id: 0, name: 'Everything' };
    if (entry) {
      console.log('visuals: set ' + inForce.id + ' ' + JSON.stringify(inForce.name) + ' entry '
        + entry.index + '/' + entry.total + ' ' + entry.sketch
        + (entry.pattern ? ' ' + entry.pattern : '') + (entry.clip ? ' ' + entry.clip : ''));
    } else {
      console.log('visuals: set ' + inForce.id + ' ' + JSON.stringify(inForce.name) + ' ' + sketch.name);
    }
    // Only an entry answers a switch into a sequence. A sketch requested
    // before the switch that lands after it must not cancel it, or entry 1
    // waits a whole Switch every interval rather than the next beat.
    if (entry) { switchWanted = false; nothingNoted = false; }
    holdSince = -1;
    // Next entry's pattern, loading while this one plays. The pattern this
    // entry's run() just took is not `held` until its first bind on the next
    // frame; patterns.js's drawing() protects it from this pin until then.
    // The idle sketch keeps the interrupted entry's pattern instead.
    if (sketch === window.idleSketch && inSequence()) keepForResume(); else prefetch();
    if (switchHook) {
      try { switchHook(sketch); } catch (e) { console.error('visuals: switch hook failed', e); }
    }
  }

  function endMelt() {
    melting = false;
    melt = 0;
    render(o0);
    // Every output still ticks each frame whatever is on it
    // (HydraRenderer.tick draws all four), so drop o2 to the cheapest chain
    // there is rather than leaving a blend and a noise running under a steady
    // picture. s3 keeps the last still; it costs nothing until it is sampled
    // again and holding it means a snapshot that fails has something to fall
    // back on.
    solid(0, 0, 0, 0).out(o2);
    if (queued) {
      const next = queued, nextEntry = entryQueued;
      queued = null; entryQueued = null;
      show(next, nextEntry);
    } else if (nextPending > 0) {
      // Drain one counted tap per melt end. pickNext() runs now, not at tap
      // time, so a settings change that landed during the wait is already
      // reflected in what the drained melt shows.
      nextPending -= 1;
      advance(true);
    }
  }

  // The rotation logic must survive sketch switches, so it subscribes with
  // onBeatAlways; onBeat is sketch-scoped and gets wiped by startPending().
  feed.onBeatAlways(() => {
    lastBeatAt = performance.now();
    // Leaving idle picks at random, except in preview mode while a sketch
    // preview.js asked for is already on its way or on screen: the boot
    // show(idleSketch) leaves idle true, and without this the first beat of
    // the mock feed would replace ?sketch=NAME half a second after it landed.
    if (idle) {
      idle = false;
      if (!paused || (current === window.idleSketch && !queued && !pending)) {
        if (inSequence()) resumeEntry(false); else show(pickNext());
      }
      return;
    }
    // Preview mode holds the sketch until someone moves it. Leaving idle
    // above still happens, so a paused page does not sit on the idle sketch.
    if (paused) return;
    // The rotation, unlike the idle and camera paths, has no reason to be
    // held: it comes round beatsPerSwitch() beats after the last switch and
    // can simply wait for the next one rather than queueing behind a melt
    // that is still running. The count is >= rather than ==, because a beat
    // that lands while busy() is true is not lost here; the switch happens
    // on the next beat instead.
    if (busy()) return;
    const due = feed.beats - currentSinceBeat >= beatsForCurrent() && performance.now() - currentSince > MIN_SKETCH_MS;
    if (switchWanted || due || holdSince >= 0) advance(false);
  });

  // Falling into idle, and nothing else: the idle sketch stays up until a
  // beat arrives and the onBeatAlways handler above switches away from it.
  setInterval(() => {
    if (!idle && performance.now() - lastBeatAt > IDLE_AFTER_MS) {
      idle = true; show(window.idleSketch);
    }
  }, 1000);

  (function logRenderer() {
    const gl = canvas.getContext('webgl2') || canvas.getContext('webgl');
    const ext = gl && gl.getExtension('WEBGL_debug_renderer_info');
    console.log('visuals: renderer', ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : 'unknown');
  })();
  let lastLog = performance.now();
  window.update = driveFrame;
  // The range each envelope covered since the last log line, sampled on every
  // rendered frame in driveFrame. This is what fork issue #4 was missing: the
  // old line said the beatgrid was arriving and nothing about whether the
  // bands were, or how far the envelopes the sketches read ever travelled.
  // Read off the line whether a reaction exists before judging its size on
  // the TV; a swell that spans 0.2 in ten seconds of a drop is a feed problem
  // and not a sketch amount problem.
  // rawBass rather than bass: the raw band says whether the sidechain heard
  // anything, and bass is already through the auto-gain, which stretches
  // whatever it hears to full scale. The line prints it as "bass".
  const RANGE_KEYS = ['rawBass', 'energy', 'swell', 'pulse', 'bounce'];
  const RANGE_LABELS = { rawBass: 'bass', energy: 'energy', swell: 'swell', pulse: 'pulse', bounce: 'bounce' };
  const range = {};
  function resetRange() {
    RANGE_KEYS.forEach((k) => { range[k] = [Infinity, -Infinity]; });
  }
  resetRange();
  // A declaration rather than an assignment so driveFrame, defined above
  // and first called by hydra after this file has finished, can reach it.
  function sampleRange() {
    RANGE_KEYS.forEach((k) => {
      const v = feed[k];
      if (v < range[k][0]) range[k][0] = v;
      if (v > range[k][1]) range[k][1] = v;
    });
  }
  setInterval(() => {
    const now = performance.now();
    const spans = RANGE_KEYS.map((k) => {
      const [lo, hi] = range[k];
      const label = RANGE_LABELS[k];
      return lo <= hi ? `${label} ${lo.toFixed(2)}..${hi.toFixed(2)}` : `${label} -`;
    }).join(' ');
    console.log('visuals: fps', (frames * 1000 / (now - lastLog)).toFixed(1),
      'feed', feed.alive ? 'alive' : 'dead',
      feed.playing ? 'playing' : 'stopped',
      'bpm', feed.bpm.toFixed(1), 'beats', feed.beats,
      'set', [feed.settings.reactivity, feed.settings.bounce, feed.settings.swirl, feed.settings.bars].join('/'),
      spans);
    frames = 0; lastLog = now;
    resetRange();
  }, LOG_EVERY_MS);

  // A new set is in force: a switch on the panel, an edit on the admin page,
  // or the file becoming unreadable. The pools behind the sketches follow at
  // once (video.refresh() rereads the uploads too, since an upload is what
  // usually comes with an edit). The sketch on screen is left alone if the
  // set still allows it, which is what the spec asks for a Random set; if
  // not, the show melts to something the set does allow. `target` rather than
  // `current`, so a switch still landing is judged too: the change can arrive
  // between show() and startPending().
  if (window.sets) {
    sets.onChange((info) => {
      if (window.video) video.refresh();
      if (window.patterns && patterns.setChanged) patterns.setChanged();
      const a = sets.active();
      console.log('visuals: sets ' + a.id + ' ' + JSON.stringify(a.name) + ' in force (' + info.reason + ')');
      const seq = inSequence();
      const left = wasSequence && !seq;
      // A switch, or the file going unreadable under a sequence, forgets the
      // entry of a switch still landing or queued. The sketch still lands,
      // but as a plain sketch: an entry of the old set landing under the new
      // one would pin its pattern where no sequence will ever unpin it, log
      // itself as one of the new set's entries, and answer a new sequence's
      // switch, so the new entry 1 would wait a whole entry. The drop before
      // landing and the Random rule below judge the plain sketch as usual.
      if (info.reason === 'switch' || left) {
        entryPending = null; entryQueued = null;
        resuming = false;
        // A new set gets its own "has nothing that can play" line.
        nothingNoted = false;
      }
      if (left) {
        if (window.patterns) patterns.unpin();
        entryNow = null; holdSince = -1; switchWanted = false;
      }
      wasSequence = seq;
      if (seq) {
        if (info.reason === 'switch') {
          sets.restart();
          holdSince = -1;
          // Entry 1's pattern starts loading now rather than at the first
          // beat, which is most of the wait for it.
          prefetch();
          // While idle, the first beat out of idle shows entry 1 (replay
          // before any next() is next()); otherwise the next beat does.
          if (!idle) switchWanted = true;
        }
        return;
      }
      if (idle) return;
      const target = queued || pending || current;
      // The sequence left behind the idle sketch with the music playing
      // (nothing in it could play, or a resume was holding for a pattern).
      // The Random rule below never moves the idle sketch, which would stay
      // up until the next due switch, so the new set starts now.
      if (left && target === window.idleSketch) {
        show(pickNext());
        return;
      }
      if (target && target !== window.idleSketch && !setAllows(target)) {
        console.log('visuals: sets ' + a.id + ' leaves out ' + target.name);
        show(pickNext());
      }
    });
  }

  // The first switch of the page has nothing behind it: the still is a blank
  // canvas, so the idle sketch melts up out of black over two seconds. That
  // is deliberate and is the boot fade; it is not the black crossfade bug.
  show(window.idleSketch);
  idle = true;

  // The handle preview.js drives the show through, and nothing else reads it.
  // preview.js returns at once unless the URL carries ?preview=1, so on the Pi
  // this object exists and is never called. `target` is where the show is
  // heading: a request held behind a melt, then one armed but not landed,
  // then the sketch on screen, so stepping twice during a melt moves two
  // places rather than one.
  window.director = {
    list: () => window.sketches,
    current: () => current,
    target: () => queued || pending || current,
    show: show,
    pickNext: pickNext,
    onSwitch(fn) { switchHook = typeof fn === 'function' ? fn : null; },
    pause(on) { paused = !!on; },
    // preview.js only: replace the webcam with `impl` ({ open(), close(),
    // label }). camReady goes true and stays true.
    useCamera(impl) {
      camImpl = impl;
      window.camReady = true;
      console.log('visuals: camera is the preview', impl && impl.label ? impl.label : 'stand-in');
    }
  };
})();
