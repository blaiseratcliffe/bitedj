// The sketch library for the HDMI visuals page: twenty-four rotation sketches
// and the idle fallback. Sixteen are drawn from oscillators, noise and the
// camera; the eight named pattern-* draw Book of Shapes SVG artwork that
// patterns.js has rasterised, morphing it along a parameter sweep. The helpers
// for those are grouped together below, after accent().
//
// The contract with director.js, which owns the rotation:
//   - every chain here ends in .out(o0). o1 is free scratch for a sketch that
//     wants a cheap intermediate pass, and several below use it to build a
//     field once instead of four times; the director blanks it on every
//     switch. o2 and the source s3 belong to the director's crossfade and
//     nothing here may write to either; s3 holds the frozen outgoing frame
//     and its texture is rewritten on every switch. o3 is written by nothing
//     today and stays the director's as well. s4 to s7 belong to patterns.js
//     and are reached through patterns.bind(), never by hand.
//   - a sketch that needs a pattern carries `pattern: true`, which keeps it
//     out of the rotation until something has finished rasterising, exactly as
//     `cam: true` keeps a sketch out until there is a camera.
//   - it clears the sketch-scoped beat listeners and sets window.sketchUpdate
//     back to null on every switch, so run() is the right and the only place
//     to call feed.onBeat(fn) or to assign window.sketchUpdate = (dt) => {}.
//     dt is milliseconds. The springs and the shared clock cover everything
//     the old per-sketch integrators did, so nothing here uses feed.onBeat;
//     the eight pattern sketches are the only users of window.sketchUpdate,
//     and all they do with it is push the sweep position at patterns.bind().
//   - window.update, window.afterUpdate and feed.onBeatAlways belong to the
//     director. Nothing here touches any of them, and nothing here calls
//     hush(), which would clear the sources as well as the outputs.
//     afterUpdate is the one that looks free and is not: the director takes
//     the crossfade's frozen frame there, because it is the only moment in a
//     frame at which the canvas can be read, and a sketch that assigned it
//     would break every switch from then on without any error anywhere.
//
// Two families. mono is white line work on pure black: no .color() beyond
// white or a grey scale factor, no fills. colour uses palette.js tokens only.
//
// How the music is allowed to move the picture. This is the whole point of
// the file and the thing the first pass got wrong, so it is worth being blunt
// about it:
//
//   - rate of motion comes from flow() below, which is a clock that runs
//     faster when feed.energy is high. Never from a band value directly.
//   - amount of warp, scale or brightness comes from feed.swell and
//     feed.energy, over ranges narrow enough that loud and quiet are
//     obviously different but a single kick cannot slam the frame.
//   - a beat is worth a few percent, through feed.pulse, or a new target for
//     a feed.spring. Never an instantaneous jump, and never a random number.
//   - no posterize or thresh level is driven by a live value. Stepping a
//     quantiser with a moving number is a flicker generator; the levels stay
//     fixed and the input moves through them instead.
//   - trails are made with blend(), not with an additive layer over a dimmed
//     copy. A mix can never be brighter than its brightest input, so however
//     long the trail is it cannot pile up into a white field.
//
// One measured number is worth carrying around while editing this file. On a
// dense field of thin lines, a ridge plot or a wire mesh, about a pixel of
// movement between two rendered frames changes four or five percent of the
// frame, because every line crosses into where it was not. That is the whole
// difference between smooth and twitchy, and it means the amplitude of a warp
// on such a field has to come from feed.swell and not from feed.energy: the
// energy envelope ripples over each beat, by about seven hundredths on the
// mock feed and by 0.3 on a real drum and bass drop captured on the box
// (feed.js replayed on a capture read energy 0.55..0.85 through a drop),
// which is small enough to be invisible as brightness and quite large enough
// to shove sixty lines a pixel sideways four times a second. Sparse bright
// things, a wordmark or a scatter of dots, are an order of magnitude less
// sensitive and can be driven from energy directly.
//
// Three hydra facts shaped the code below.
//
// Every transform mutates the chain it is called on and returns that same
// object (GlslSource.prototype[method] pushes onto this.transforms and
// returns this). So a helper that needs the same source more than once has to
// be handed a factory that builds a fresh chain per call, never a built
// chain; edges() below takes a factory for exactly that reason.
//
// A source texture is always stretched over the whole output quad, 960x540
// here, so a source whose own aspect differs has to be corrected through the
// xMult
// argument of scale(amount, xMult, yMult), which magnifies by amount * xMult
// in x and amount * yMult in y. The correction factor is the source aspect
// over the canvas aspect.
//
// The last coordinate transform in a chain is applied to the screen
// coordinate first (generate-glsl.js emits `uv = f(uv)` and then the rest of
// the chain), so a screen space warp such as the terrain perspective goes at
// the end of the chain, and a warp in source space goes near the front. A
// combine's second input is generated against the coordinate as it stands
// where the combine sits, which is what lets smear() zoom the feedback copy
// without dragging the new line work along with it.
(function () {
  // (1024 / 256) / (960 / 540). The wordmark text nearly fills its texture,
  // so it spans the whole screen at an amount near 0.44 and crops at the
  // sides above that; the sketches sit around 0.33 for a comfortable margin.
  const MARK_X = (1024 / 256) / (960 / 540);
  // (964 / 135) / (960 / 540). Full width at an amount near 0.25. The boot
  // logo PNG is 964 wide, not the 960 of the spec; measured off the IHDR.
  const LOGO_X = (964 / 135) / (960 / 540);

  // The boot logo PNG is NOT transparent. Every pixel of assets/boot-logo.png
  // has alpha 255 (read straight out of the IDAT: the alpha histogram is a
  // single bucket at 255), and the background around the letters is solid
  // black. Rendering src(s2) over a red field shows no red anywhere, so
  // layer() has nothing to composite against and the logo arrives as a black
  // rectangle with type in it.
  //
  // So the alpha has to be made rather than used. This keys out the black and
  // leaves everything else. The threshold sits between pure black, luminance
  // 0, and the darkest stripe in the artwork, palette deep #240f2d, whose
  // luminance is 0.2126*36 + 0.7152*15 + 0.0722*45 over 255, about 0.085. At
  // 0.035 with a tolerance of 0.015 the band runs 0.02 to 0.05: comfortably
  // above black, comfortably below the darkest thing worth keeping, and
  // narrow enough that the antialiased edge of the white outline still gets a
  // soft ramp rather than a hard cut.
  const LOGO_KEY = [0.035, 0.015];

  // osc(freq) is sin(st * freq), so the number of lines across the coordinate
  // is freq / TAU. Every line count below is written as COUNT * TAU, because
  // a bare frequency is impossible to read as a line count and the first
  // versions of the ridge and terrain sketches came out at a quarter of the
  // density they were meant to have.
  const TAU = 2 * Math.PI;

  // The shared clock. Every sketch that used to read hydra.synth.time reads
  // flow() instead: the same seconds, but advancing between about a third of
  // real time when the room is quiet and one and a half times it when the
  // music is loud. The rate is what energy scales, not the value: multiplying
  // an absolute clock by a changing number makes the picture jump backwards
  // and forwards, which is the fault this whole rework exists to remove.
  //
  // It runs on its own requestAnimationFrame rather than on the director's
  // frame drive so that a sketch does not have to own an integrator to have a
  // clock, and so that every sketch shares one.
  //
  // It wraps at FLOW_PERIOD, and the value of that constant is the whole
  // reason this comment is long. A clock that only ever grows reaches a few
  // thousand after an hour or two of a set, and every consumer here hands it
  // to a shader as a float32 uniform: at 6000 the spacing between
  // representable floats is about a thousandth, at a hundred thousand it is
  // a fiftieth of a radian and a slow rotation visibly steps. Wrapping is
  // free as long as the wrap is invisible, which means the period has to be a
  // whole number of cycles for every consumer. TAU * 1000 is, because the
  // consumers are all of the form sin(flow() * rate) or rotate(flow() * rate)
  // and every rate in the file is a multiple of a thousandth: the wrap moves
  // each of them by an exact multiple of a full turn.
  //
  // So: any new rate multiplied into flow() must be a multiple of 0.001. A
  // rate of 0.0333 would put a jump into that sketch once every two hours,
  // which is the kind of fault nobody finds. A rate that feeds a scroll
  // rather than an angle has a different rule, since a scroll counts turns
  // and not radians: it has to be a whole number of turns per period, which
  // is what the one such rate in the file, in scan-field, is written as.
  const FLOW_PERIOD = TAU * 1000;
  let flowT = 0, flowAt = performance.now();
  function flowTick() {
    const now = performance.now();
    const dt = Math.min(100, now - flowAt);
    flowAt = now;
    flowT += (dt / 1000) * (0.35 + 1.15 * (window.feed ? feed.energy : 0));
    if (flowT >= FLOW_PERIOD) flowT -= FLOW_PERIOD;
    requestAnimationFrame(flowTick);
  }
  requestAnimationFrame(flowTick);
  const flow = () => flowT;

  // Edges of a source: the difference against copies of itself shifted by a
  // pixel in x and y, then a threshold. hydra's custom GLSL cannot sample
  // textures, so this is the whole edge detector.
  //
  // `make` is a factory, not a chain: the detector needs four independent
  // copies of the source and hydra transforms mutate in place, so passing one
  // chain would splice the shifts and the diff into the very thing being
  // differenced. `gain` may be a number or a function, but it feeds a fixed
  // threshold, so a gain that swings with the music switches whole edges on
  // and off. Every caller below keeps it nearly constant for that reason.
  //
  // The shift is one pixel of whatever the output actually is, read per
  // frame rather than assumed, because a one-pixel step is the whole point:
  // hardcoding 1/960 against a smaller render target shifts by less than a
  // pixel and the difference comes out empty.
  function edges(make, gain) {
    const px = () => 1 / (hydra.synth.width || 960);
    const py = () => 1 / (hydra.synth.height || 540);
    return make().diff(make().scrollX(px))
      .add(make().diff(make().scrollY(py)))
      .luma(0.02, 0.01)
      .mult(solid(gain, gain, gain, 1))
      .thresh(0.15, 0.05);
  }

  // New work mixed over the previous frame, blown up by a hair so the history
  // drifts outward: the main tool for the liquid feel in this file.
  //
  // blend, not layer over a dimmed copy. The old trail() fed the previous
  // frame back at a gain and added new line work on top, which is a geometric
  // series: at any fade above about 0.8 the bright pixels converge on white
  // and the frame blooms. A mix is bounded by its brightest input by
  // construction, so `keep` can go to 0.95 for a long smear with no risk at
  // all. `keep` is how much of the previous frame survives one frame, `grow`
  // the zoom per frame, which wants to stay within a few thousandths of 1.
  //
  // The work is composited onto black before it is mixed in, and that line is
  // not optional. blend() is a straight mix of all four channels,
  // c0 * (1 - amount) + c1 * amount, alpha included: it is not a composite,
  // so it never weights a colour by its own alpha. The wordmark texture
  // carries its glow entirely in alpha, every pixel within 24 px of a stroke
  // being rgb 255 at an alpha around 48, because that is what a canvas shadow
  // is. Mixed channel by channel that whole halo is white and the wordmark
  // arrives as a solid slab with no letterforms in it at all, which is
  // exactly how the first version of this rework looked; the alpha the mix
  // carried along made no difference, because nothing downstream reads it.
  // layer() over solid(0,0,0,1) is mix(black, work.rgb, work.a), which is the
  // premultiply, and it is a no-op for the line work and edge chains whose
  // alpha is already 1 wherever they are lit.
  function smear(work, keep, grow) {
    return src(o0).scale(grow).blend(solid(0, 0, 0, 1).layer(work), 1 - keep);
  }

  // Alpha-key a filled black and white image so it can be layered: luma sets
  // alpha from luminance, so the black stays out of the way.
  function keyed(chain) {
    return chain.luma(0.5, 0.06);
  }

  // hydra samples every source as texture2D(tex, fract(st)) (the `src`
  // transform, vendor/hydra-synth.js:1939), so any chain that scales a source
  // down to less than the full quad sees it tiled rather than running out
  // into nothing: the wordmark at 0.42 comes up three times stacked. This
  // mask is white inside the source's own 0..1 square and black outside it.
  //
  // Placed with .mask() *before* the .scale() in the chain it is evaluated in
  // source coordinates rather than screen coordinates, which is what makes it
  // independent of whatever the scale amount is doing that frame; hydra emits
  // a combine's input against the coordinate as it stands at that point, and
  // coordinate transforms later in the chain have already been applied by
  // then. mask(), not mult(): mask multiplies alpha as well, and the stray
  // copies have to lose their alpha or they punch black holes in a layer().
  //
  // gradient(0) carries st.x in red and st.y in green. The diff against a
  // half grey is the distance from the middle of that axis, invert turns it
  // into a luminance that falls as the distance grows, and the threshold cuts
  // at the half width, scaled by that channel's weight in the luminance sum.
  const LUMA_R = 0.2126, LUMA_G = 0.7152;
  function oneTile() {
    const inX = gradient(0).mult(solid(1, 0, 0, 1)).diff(solid(0.5, 0, 0, 1))
      .invert().thresh(1 - LUMA_R * 0.5, 0.002);
    const inY = gradient(0).mult(solid(0, 1, 0, 1)).diff(solid(0, 0.5, 0, 1))
      .invert().thresh(1 - LUMA_G * 0.5, 0.004);
    return inX.mult(inY);
  }

  // The wordmark as a single centred copy at `amount`, aspect corrected.
  function wordmark(amount) {
    return src(s1).mask(oneTile()).scale(amount, MARK_X, 1);
  }

  // A grey, for .mult(solid(...)), that sits near one and lifts a few percent
  // on the beat. This is what a kick is worth in brightness.
  function accent(base, lift) {
    return () => base + lift * feed.pulse;
  }

  // What a kick is worth in motion. kick(amount) is a multiplier that sits
  // at 1 and follows feed.bounce, which snaps to 1 on the beat edge,
  // overshoots below zero and settles inside a beat; every sketch below
  // multiplies it into the scale of its main element and nothing else, so
  // the foreground pops on the kick and the background keeps its drift.
  // Blaise's verdict on the smoothness rework was that it went too far:
  // "too smooth / liquidy, they need to have some bounce in them too".
  //
  // Two sizes. 0.08 on a scale is an eight percent pop, visible on a
  // wordmark or a piece of artwork and the size of a speaker cone moving.
  // 0.4 on a warp amplitude is a forty percent change in a number that is
  // itself a few hundredths of the frame, which is what it takes for a
  // line field to be seen to kick without the whole field jumping a pixel;
  // the header explains why those fields cannot take the 0.08 treatment.
  // BOUNCE scales every amount at once, and is the number to change from
  // the TV.
  const BOUNCE = 1;
  function kick(amount) {
    return () => 1 + BOUNCE * amount * feed.bounce;
  }
  const KICK_SCALE = 0.08;
  const KICK_WARP = 0.4;

  // ---- the Book of Shapes pattern family ---------------------------------
  //
  // patterns.js rasterises a pattern's seven SVG frames into 1024x1024
  // canvases and binds two of them to a slot's pair of sources: slot 0 is s4
  // and s5, slot 1 is s6 and s7. A sketch here does three things and nothing
  // else about loading: ask for a pattern with patterns.take(tags), drive
  // patterns.bind() from window.sketchUpdate, and draw sweepPair().
  //
  // take() returns null only when nothing has finished rasterising.
  // director.js already keeps a pattern sketch out of the rotation until
  // patterns.ready(), so the fallback below is the belt and not the braces.
  //
  // The texture is square and the output is 16:9, and hydra stretches any
  // source over the whole quad, so the square has to be squeezed back. For the
  // artwork to keep its proportions the visible source region must be the full
  // width of the texture and 540/960 of its height, which is
  // scale(1, 1, 960/540): it crops the top and the bottom rather than
  // pillarboxing. Every one of these patterns is a centred composition with
  // margin around it, so the crop takes the margin.
  const SQUARE_Y = 960 / 540;

  // The two bound frames, mixed by the fractional part of the sweep position,
  // which is what makes a still SVG breathe through its own slider. The two
  // sources are sampled at the same coordinate: both sit before every
  // coordinate transform the caller adds, so a scroll or a rotate moves the
  // pair together rather than sliding one frame against the other.
  function sweepPair(slot) {
    const i = slot ? 1 : 0;
    return src(i ? s6 : s4).blend(src(i ? s7 : s5), () => patterns.mix[i]);
  }

  // These patterns do not tile. They are single compositions inside a square
  // with a margin, so a scroll that wraps drags the left edge of the artwork
  // against its right edge and a hard seam crosses the frame. Every drift in
  // this family is therefore bounded: a slow sine of a few percent of the
  // frame, which never reaches the wrap. Only kaleid() and the rotations are
  // allowed to sample outside the square, where the repeat reads as part of
  // the fold rather than as a tear.
  function driftX(amount, rate, phase) {
    return () => amount * Math.sin(flow() * rate + (phase || 0));
  }

  // The beat clock: beats elapsed, integrated on the render clock at the
  // master deck's tempo, wrapping at TURN_BEATS so the number stays small and
  // the wrap is exactly one revolution for anything that divides by it.
  //
  // The first version of pattern-grid read `(feed.beats % 32) + feed.phase`
  // instead, and that is not a clock. feed.beats increments on the beat edge
  // the deck reports and feed.phase free-runs on the render clock and is
  // corrected forward by the deck's beat_distance, so the two are not coupled
  // at all: phase can already be a tenth of a beat past 1 when the integer
  // arrives, or the integer can arrive first. Either way the sum steps, by up
  // to a tenth of a beat, which at one revolution per 32 beats is 11 degrees
  // of a dense grid in a single frame, once a beat, forever. Integrating
  // through feed.beatMs() gives the same revolutions per bar with nothing to
  // step: the tempo changes the rate, and a rate change is not a jump.
  //
  // 256 beats a revolution, not the 32 the sketch was first written with.
  // 32 beats is eleven seconds at 174, which moves the corner of the frame
  // about twelve pixels between rendered frames: on a pattern with two fifths
  // of the frame lit that measured 2.7 to 3.0 percent frame to frame against
  // a target of 1.5, and the two ways of buying that back both cost the
  // picture. A longer trail did meet the number, at 1.3, and what came back
  // was a grey motion-blurred disc with no line in it anywhere; the
  // screenshots of both are in the report. At 256 beats, 88 s a revolution,
  // the same pattern measures 0.8 to 2.5 with a short trail and stays visibly
  // line work. It reads as a slow lean rather than a spin, which is the
  // trade: this family is line work first.
  const TURN_BEATS = 256;
  let beatT = 0, beatAt = performance.now();
  function beatTick() {
    const now = performance.now();
    const dt = Math.min(100, now - beatAt);
    beatAt = now;
    beatT += dt / (window.feed ? feed.beatMs() : 60000 / 174);
    if (beatT >= TURN_BEATS) beatT -= TURN_BEATS;
    requestAnimationFrame(beatTick);
  }
  requestAnimationFrame(beatTick);
  const beats = () => beatT;

  // No pattern, no black screen: the wordmark is in memory from the first
  // frame the page ever drew.
  function patternFallback() {
    smear(wordmark(0.3).mult(solid(0.85, 0.85, 0.85, 1)), 0.85, 1.003).out(o0);
  }

  const monochrome = [

    // 1. The wordmark breathing. The size follows the swell, so it is a
    // little larger through a loud passage and a little smaller through a
    // break, and the outward smear turns the strokes into a soft glow that
    // never resolves to a hard edge. The beat is four percent of brightness
    // and half a percent of size: present, not an event.
    { name: 'logo-outline', cam: false, mono: true, run() {
        const bright = accent(0.86, 0.1);
        const size = () => (0.325 + 0.02 * feed.swell) * kick(KICK_SCALE)();
        smear(wordmark(size)
          .scrollY(() => 0.012 * Math.sin(flow() * 0.23))
          .rotate(() => 0.035 * Math.sin(flow() * 0.5))
          .mult(solid(bright, bright, bright, 1)), 0.85, 1.003)
          .out(o0);
    } },

    // 2. The logotype mirrored four ways around the centre, letters readable,
    // turning continuously at a rate set by energy. A beat adds a couple of
    // degrees of extra turn through a spring, so the ring leans into the kick
    // and settles back; the old version added a fixed fifteen degrees to a
    // counter on every beat, which is a jump however small the step.
    //
    // Both numbers are small for the same reason: the letters sit at about
    // four tenths of the frame from the centre, so a degree of turn moves
    // them seven pixels, and thin strokes moving seven pixels between two
    // rendered frames is a twitch even when the cause of it is a spring.
    //
    // The short smear on the way out is doing more than it looks. A trail is
    // a low pass filter in time as well as a look: measured, the same
    // rotation went from six and a half percent of the frame changing between
    // rendered frames to under two, because the ring is now a soft edge
    // rather than a hard one and a soft edge moving a pixel changes a pixel
    // by very little. It is the cheapest way to buy motion back on anything
    // drawn in thin lines.
    //
    // Built out of four placed copies, not out of kaleid(). kaleid(4) squeezes
    // a full turn into a 45 degree wedge, which stretches everything eight
    // times tangentially: letterforms come out as solid white slabs and the
    // wordmark is not recognisable at all. Four copies of a single wordmark,
    // pushed off centre and rotated a quarter turn apart, give the same
    // fourfold symmetry with the type still legible.
    //
    // The base of the stack is solid(0,0,0,0) rather than the first copy.
    // layer(c0, c1) mixes by c1's alpha only, so whatever sits at the bottom
    // contributes its rgb unweighted, and the wordmark's soft glow would come
    // through as a solid white slab where the layers above are transparent.
    { name: 'logo-kaleid', cam: false, mono: true, run() {
        wordmark(() => (0.255 + 0.012 * feed.swell) * kick(KICK_SCALE)()).out(o1);
        const arm = (a) => src(o1)
          .scrollY(() => -0.28 - 0.016 * feed.swell)
          .rotate(a);
        const turn = () => flow() * 0.025
          + 0.03 * feed.spring('kaleid-kick', () => feed.pulse, 1);
        smear(solid(0, 0, 0, 0)
          .layer(arm(0))
          .layer(arm(Math.PI / 2))
          .layer(arm(Math.PI))
          .layer(arm(-Math.PI / 2))
          .rotate(turn), 0.8, 1.0012)
          .out(o0);
    } },

    // 3. The wordmark sheared. This was logo-sliced, a slicer: a beat threw a
    // random offset into a posterised band modulator and it decayed over
    // 300 ms, so the type tore itself apart four times a second at a
    // different place every time. It is now one continuous shear, and it is
    // named for that rather than for what it used to do. The modulator is a
    // smooth
    // vertical wave rather than a stepped ramp, leaning one way and then the
    // other on the shared clock. The lean opens up with energy, so a loud
    // passage pulls the letters further out of true, and the beat is worth
    // one percent of it.
    //
    // The modulator carries its ramp in the red channel only, with green
    // zeroed, so modulate() displaces x and leaves y alone; modulate rather
    // than modulateScrollX because the scroll variants fract the coordinate
    // and the wordmark would wrap instead of sliding. brightness(-0.5)
    // centres the red channel on zero so the shear goes both ways.
    { name: 'logo-shear', cam: false, mono: true, run() {
        const wave = () => osc(4.2, 0.07, 0).rotate(Math.PI / 2)
          .brightness(-0.5).mult(solid(1, 0, 0, 1));
        const lean = () => 0.07 + 0.06 * Math.sin(flow() * 0.9)
          + 0.04 * feed.energy + 0.01 * feed.pulse;
        const bright = accent(0.88, 0.08);
        smear(wordmark(() => 0.34 * kick(KICK_SCALE)())
          .modulate(wave(), lean)
          .mult(solid(bright, bright, bright, 1)), 0.88, 1.0015)
          .out(o0);
    } },

    // 4. Joy Division ridges: sixty thin horizontal lines, evenly spaced and
    // mostly flat, with local bumps where the field pushes them up. The bump
    // height follows the swell, so the ridge plot opens out over a whole
    // passage rather than heaving on every kick.
    //
    // Three things make this read as ridges rather than as marbling. The
    // oscillator frequency is in radians across the coordinate, so LINES * TAU
    // is the honest way to ask for a given number of lines; a bare osc(110)
    // is 17 lines, thick and few. The threshold is fixed and tight, which
    // holds the lines at a couple of pixels instead of letting them swell into
    // bands. And the displacement field is gated by a thresholded second noise
    // so it is near zero over most of the frame: an ungated field bends every
    // line everywhere and the result is contour soup, not a ridge plot.
    //
    // The modulator's red channel is zeroed, so the displacement is in y only
    // and the lines never slide along their own length.
    { name: 'ridge-lines', cam: false, mono: true, run() {
        const bumps = () => noise(4.5, 0.006)
          .mult(noise(2.2, 0.005).thresh(0.3, 0.3))
          .mult(solid(0, 1, 0, 1));
        osc(60 * TAU, 0, 0).rotate(Math.PI / 2)
          .modulate(bumps(), () => (0.02 + 0.05 * feed.swell) * kick(KICK_WARP)())
          .thresh(0.93, 0.01)
          .out(o0);
    } },

    // 5. A bundle of some thirty-five thin lines bending together, laid over
    // a long smear so the bundle reads as ribbon rather than as fence. The
    // bundle's own angle drifts a couple of degrees either side of 20 on the
    // shared clock, which is what keeps the smear from settling into a static
    // blur, and the bend is the noise field moving through it rather than the
    // lines sliding along themselves.
    //
    // Three numbers do all the work. The count: osc(60) is not sixty lines,
    // it is sixty radians, which is nine and a half, and nine fat bands is not
    // a bundle. The smear: with the old additive trail at fade 0.84 the copies
    // piled up into fat white bands and the frame went two thirds white.
    // Through blend() the same 0.9 is safe, because the result cannot exceed
    // the brightest input.
    //
    // And the sync argument, which is the one that decided whether this
    // sketch was smooth. osc's second argument shifts the pattern by
    // sync * time in the coordinate, independent of the frequency, so the old
    // 0.05 slid this whole field 48 px a second across the render target: a
    // dense line field moving a pixel between rendered frames is the single
    // largest source of frame to frame change in the library. At 0.0015 the
    // lines are near enough still and everything you see moving is the warp.
    { name: 'ribbons', cam: false, mono: true, run() {
        const work = osc(34 * TAU, 0.0015, 0)
          .modulate(noise(1.4, 0.006), () => (0.08 + 0.06 * feed.swell) * kick(KICK_WARP)())
          .rotate(() => 0.35 + 0.02 * Math.sin(flow() * 0.17))
          .thresh(0.92, 0.02);
        smear(work, 0.92, 1.0008).out(o0);
    } },

    // 6. Topographic contours. The posterised noise field is rendered once
    // into o1 and the edge detector then runs on that texture, which costs
    // one noise pair per frame instead of the eight a four-copy chain would
    // need. The field drifts with energy through the warp amount; the band
    // count and the edge gain are both fixed, because both feed thresholds
    // and a threshold moved by the music adds and drops whole contour lines
    // at a time.
    { name: 'contours', cam: false, mono: true, run() {
        noise(2.6, 0.01)
          .modulate(noise(1.1, 0.006), () => (0.1 + 0.16 * feed.swell) * kick(KICK_WARP)())
          .posterize(10, 1)
          .out(o1);
        edges(() => src(o1), () => 3 + 0.4 * feed.pulse).out(o0);
    } },

    // 7. A flow field made entirely of feedback: every frame the previous
    // frame is displaced along a noise vector field and dimmed a little, and
    // sparse bright points are seeded into it, so the points draw their own
    // trajectories as lines with a dot at the head.
    //
    // The beat rides the seed *brightness* through a spring, not the seed
    // threshold. Moving the threshold is what the first version did, and it
    // switched whole dots into existence at once; moving the brightness fades
    // the same dots up and down.
    { name: 'flow-lines', cam: false, mono: true, run() {
        const seed = () => 0.8 + 0.2 * feed.spring('flow-seed', () => feed.pulse, 1.6);
        src(o0)
          .modulate(noise(3.2, 0.03), () => (0.004 + 0.008 * feed.energy) * kick(KICK_WARP)())
          .mult(solid(0.965, 0.965, 0.965, 1))
          .layer(keyed(noise(22, 0.06).thresh(0.8, 0.02))
            .mult(solid(seed, seed, seed, 1)))
          .out(o0);
    } },

    // 8. Wireframe terrain: a thirty-two by twenty mesh of single pixel lines
    // warped by a smooth field and compressing toward the horizon.
    //
    // The rows no longer come at the viewer at all, and that is the one
    // structural thing this rework took away rather than smoothed. A single
    // pixel line is the most motion-sensitive thing that can be drawn: the
    // mesh at the old sync of 0.012, about twelve pixels a second, changed a
    // quarter of the frame every second, and even a tenth of that measured
    // three times the budget for the whole sketch. Softening the lines to
    // spread the change over more pixels made it worse, not better, because
    // more of the frame is then line. So the mesh stands still and the warp
    // field moves through it, which is the same picture with the travel taken
    // out of the grid and put into the terrain.
    //
    // The warp amplitude is the thing to keep small. At 0.22 the coordinate
    // gradient folds over in places and whole cells collapse into white
    // blobs; a tenth of that bends the mesh without ever folding it, and the
    // swell moves it over a bar rather than over a kick.
    //
    // The rows scroll away on the oscillator's own sync argument rather than
    // on scrollY. scrollY ends in fract(st), and although the grid itself is
    // exactly periodic the noise warp sampled either side of the wrap is not,
    // so a hard seam of torn cells crossed the frame once a second.
    //
    // modulateScale is last in the chain, so it works on the screen
    // coordinate. Its ramp comes from a rotated gradient rather than from a
    // low frequency oscillator: gradient gives a true 0 to 1 in red, which is
    // the channel modulateScale reads, where osc(1) only covers 0.5 to 0.92
    // and barely leans the grid at all.
    { name: 'wire-terrain', cam: false, mono: true, run() {
        const yRamp = () => gradient(0).rotate(Math.PI / 2);
        osc(32 * TAU, 0, 0).thresh(0.984, 0.006)
          .add(osc(20 * TAU, 0, 0).rotate(Math.PI / 2).thresh(0.984, 0.006))
          .modulate(noise(1.6, 0.006), () => 0.02 + 0.035 * feed.swell)
          .modulateScale(yRamp(), () => 1.4 * kick(KICK_SCALE)(), 1.0)
          .out(o0);
    } },

    // 9. Webcam edges over a long smear, so the outline leaves a wake behind
    // whoever is moving rather than crackling in place. The gain is nearly
    // constant for the reason given at edges(): it feeds a threshold, and an
    // edge that already clears the threshold does not get any brighter for
    // being multiplied harder, it just brings in and drops out the marginal
    // ones. The swell lengthens the wake instead, which is the part that
    // actually reads.
    { name: 'cam-edges', cam: true, mono: true, run() {
        smear(edges(() => src(s0), () => 3.5 + 0.5 * feed.pulse).scale(kick(KICK_SCALE)),
          0.82, () => 1.002 + 0.003 * feed.swell)
          .out(o0);
    } },

    // 10. Webcam luminance posterised into a handful of bands and reduced to
    // the band edges, a contour map of whoever is in front of the lens.
    // Same o1 trick as contours: posterise once, detect edges on the result.
    //
    // The band count is fixed at six. Riding it on the bass, as the first
    // version did, added and dropped whole contour lines on the kick, which
    // is the most literal version of the flicker this file is trying to be
    // rid of. The bands still move, because the image is lifted and contrast
    // stretched slowly underneath them, so the contours slide over the face
    // instead of blinking on and off.
    { name: 'cam-contours', cam: true, mono: true, run() {
        src(s0).saturate(0)
          .brightness(() => 0.02 * Math.sin(flow() * 0.37) + 0.015 * feed.energy)
          .contrast(() => 1 + 0.1 * feed.swell)
          .posterize(6, 1)
          .out(o1);
        edges(() => src(o1), 3).scale(kick(KICK_SCALE)).out(o0);
    } },

    // 11. A flow or physics pattern breathing through its sweep, drifting on
    // two slow sines at different rates so the path never repeats, over a long
    // smear. The smear is doing the same job it does for ribbons: these
    // patterns are dense thin line work, and a soft edge moving a pixel
    // changes far less of the frame than a hard one.
    { name: 'pattern-flow', cam: false, mono: true, pattern: true, run() {
        const p = patterns.take(['FLOW', 'PHYSICS']);
        if (!p) { patternFallback(); return; }
        window.sketchUpdate = () => patterns.bind(p, patterns.sweepT());
        const bright = accent(0.92, 0.06);
        smear(sweepPair(0)
          .scrollX(driftX(0.02, 0.05))
          .scrollY(driftX(0.014, 0.037, 1.3))
          .scale(kick(KICK_SCALE), 1, SQUARE_Y)
          .mult(solid(bright, bright, bright, 1)), 0.92, 1.0004)
          .out(o0);
    } },

    // 12. A grid or isometric pattern turning once every 256 beats, which is
    // 64 bars and about 88 s at 174: slow enough that the motion is felt
    // rather than watched, and locked to the tempo rather than to a clock, so
    // it comes round on a bar line at any BPM. The angle comes off beats(),
    // the tempo-integrated accumulator above, which also carries the reason
    // the rate is 256 and not the eight bars this was first written with.
    //
    // This is the widest spread in the family, 0.8 to 2.5 percent frame to
    // frame across three runs on the same pattern, and the cause is the
    // tiling rather than the rotation: the texture is square and the zoom is
    // 1.3, so the corners sample outside it and hydra's fract() puts a copy
    // of the artwork there. As the rotation carries a tile boundary across the
    // frame, a band of the picture changes all at once. Masking to one tile
    // the way wordmark() does would fix it and would put black corners on a
    // turning frame, which is a different sketch.
    //
    // modulateScale is last in the chain, so the perspective lean works on the
    // screen coordinate; its ramp is a rotated gradient for the reason given
    // at wire-terrain, that gradient's red channel is a true 0 to 1 where an
    // oscillator's covers half that. The zoom past 1 is what keeps the fold
    // at the corners of a rotating square texture off the screen for most of
    // the turn; where it does reach outside, a grid repeating is a grid.
    { name: 'pattern-grid', cam: false, mono: true, pattern: true, run() {
        const p = patterns.take(['GRID', 'ISOMETRIC']);
        if (!p) { patternFallback(); return; }
        window.sketchUpdate = () => patterns.bind(p, patterns.sweepT());
        const yRamp = () => gradient(0).rotate(Math.PI / 2);
        const turn = () => 2 * Math.PI * beats() / TURN_BEATS;
        smear(sweepPair(0)
          .rotate(turn)
          .scale(() => (1.3 + 0.06 * feed.swell) * kick(KICK_SCALE)(), 1, SQUARE_Y)
          .modulateScale(yRamp(), () => 0.15 + 0.35 * feed.swell, 1.0), 0.85, 1.0004)
          .out(o0);
    } },

    // 13. A radial pattern folded two or three ways. The fold count is drawn
    // once per run and then left alone: a kaleid whose nSides moved with the
    // music would rebuild the whole frame on every change, which is the
    // stepping fault this library is built to avoid, and two runs of the same
    // sketch on different nights should not look identical either.
    //
    // kaleid is the last coordinate transform, so it folds the screen and
    // everything before it happens inside one wedge.
    { name: 'pattern-radial', cam: false, mono: true, pattern: true, run() {
        const p = patterns.take(['RADIAL']);
        if (!p) { patternFallback(); return; }
        window.sketchUpdate = () => patterns.bind(p, patterns.sweepT());
        const sides = 2 + Math.floor(Math.random() * 2);
        const bright = accent(0.84, 0.12);
        smear(sweepPair(0)
          .scale(() => (1.05 + 0.06 * feed.swell) * kick(KICK_SCALE)(), 1, SQUARE_Y)
          .rotate(() => flow() * 0.008)
          .kaleid(sides)
          .mult(solid(bright, bright, bright, 1)), 0.84, 1.0004)
          .out(o0);
    } },

    // 14. A noise or organic pattern under a noise warp, so the contours of
    // the artwork wander like the ones contours() draws from scratch.
    //
    // The warp amplitude is mostly swell and only a hundredth of energy,
    // against the brief's "amplitude from energy", for the reason in the
    // header: energy ripples by about seven hundredths over every beat at 174,
    // which is invisible as brightness and quite enough to shove a dense line
    // field a pixel sideways four times a second. The swell carries the shape
    // of a build and none of that ripple, which is the same argument the
    // scanline spacing in scan-field puts through a spring.
    { name: 'pattern-noise', cam: false, mono: true, pattern: true, run() {
        const p = patterns.take(['NOISE', 'ORGANIC']);
        if (!p) { patternFallback(); return; }
        window.sketchUpdate = () => patterns.bind(p, patterns.sweepT());
        smear(sweepPair(0)
          .modulate(noise(2.1, 0.008), () => 0.025 + 0.07 * feed.swell + 0.01 * feed.energy)
          .scale(kick(KICK_SCALE), 1, SQUARE_Y), 0.86, 1.0004)
          .out(o0);
    } },

    // 15. The pattern at its default settings against the far end of its own
    // sweep, crossfading back and forth under a long smear that blows the
    // history up by four thousandths a frame. The two frames are usually the
    // same artwork at very different densities, so the mix reads as one
    // drawing dissolving into another of itself.
    //
    // The wave is a raised cosine rather than the brief's triangle. A triangle
    // is continuous in value but not in rate, and a mix that reverses at a
    // corner is visible on line work as a flick; the cosine reverses at zero
    // speed. The smear is the file's own smear(), reading o0 rather than the
    // brief's o1, because o1 is scratch that the director blanks on every
    // switch and o0 is where the feedback in this file has always lived.
    //
    // The drift and the slow turn were added after the first version was
    // looked at rather than measured: at 0.02 percent between rendered frames
    // it was the smoothest thing in the library and it read as a photograph.
    // A crossfade that takes 40 s is not motion to the eye, whatever the
    // numbers say, so the frame now also creeps and leans.
    { name: 'pattern-melt', cam: false, mono: true, pattern: true, run() {
        const p = patterns.take([]);
        if (!p) { patternFallback(); return; }
        const wave = () => 0.5 - 0.5 * Math.cos(flow() * 0.22);
        const base = patterns.base(), top = patterns.top();
        window.sketchUpdate = () => patterns.bindFrames(p, base, top, wave(), 0);
        smear(sweepPair(0)
          .scrollX(driftX(0.03, 0.041))
          .scrollY(driftX(0.02, 0.029, 2.1))
          .rotate(() => 0.05 * Math.sin(flow() * 0.017))
          .scale(() => (1.05 + 0.04 * feed.swell) * kick(KICK_SCALE)(), 1, SQUARE_Y), 0.92, 1.0008)
          .out(o0);
    } },

    // 16. The same edge detector the contour sketches use, run over a pattern
    // instead of over a noise field: every line in the artwork comes back as
    // the pair of lines that bound it, which doubles a sparse drawing and
    // turns a dense one into moire. The pattern is rendered into o1 first so
    // the detector's four copies cost one texture read each rather than
    // rebuilding the two-source blend four times.
    { name: 'pattern-edges', cam: false, mono: true, pattern: true, run() {
        const p = patterns.take(['DISTORTION', 'ORGANIC']);
        if (!p) { patternFallback(); return; }
        window.sketchUpdate = () => patterns.bind(p, patterns.sweepT());
        sweepPair(0)
          .scrollX(driftX(0.012, 0.04))
          .scale(() => (1.02 + 0.05 * feed.swell) * kick(KICK_SCALE)(), 1, SQUARE_Y)
          .out(o1);
        edges(() => src(o1), () => 3 + 0.4 * feed.pulse).out(o0);
    } },

    // 17. Two patterns from different tag groups at half and half, drifting
    // and turning against each other. The second one runs its sweep backwards,
    // so the two are at opposite ends of their parameters whenever they are
    // not crossing in the middle.
    //
    // Both slots exist for this sketch and this sketch only: it is the one
    // thing in the library that needs four pattern sources. If the cache holds
    // just one pattern the same one goes in both slots, where the opposed
    // sweeps and rotations still give it something to interfere with.
    { name: 'pattern-stack', cam: false, mono: true, pattern: true, run() {
        const a = patterns.take(['GRID', 'ISOMETRIC', 'RADIAL']);
        if (!a) { patternFallback(); return; }
        const b = patterns.take(['FLOW', 'NOISE', 'ORGANIC', 'PHYSICS'], a) || a;
        window.sketchUpdate = () => {
          const t = patterns.sweepT();
          patterns.bind(a, t, 0);
          patterns.bind(b, 5 - t, 1);
        };
        // The two layers kick in opposite directions, so a beat pulls them
        // apart and lets them fall back together.
        const left = sweepPair(0)
          .scrollX(driftX(0.018, 0.045))
          .rotate(() => flow() * 0.004)
          .scale(kick(KICK_SCALE), 1, SQUARE_Y);
        const right = sweepPair(1)
          .scrollX(driftX(-0.018, 0.045))
          .rotate(() => -flow() * 0.004)
          .scale(() => 1.08 * kick(-KICK_SCALE)(), 1, SQUARE_Y);
        smear(left.blend(right, 0.5), 0.88, 1.0004).out(o0);
    } }

  ];

  const colour = [

    // 11. The boot logo, centred and readable across about eighty percent of
    // the width, over a palette field that folds through kaleid(2).
    //
    // The fold used to be a hard cut, on for every frame the bass was over a
    // threshold and off otherwise, which flipped the whole background several
    // times a bar. kaleid has no identity value of nSides, so the two chains
    // still have to exist separately, but they are cross-dissolved on the
    // swell now: mostly flat through a quiet passage, mostly folded through a
    // loud one, and the journey between them takes bars.
    //
    // The fold is on the field, never on the logo. kaleid returns
    // r * vec2(cos a, sin a), a radial remap centred on 0 rather than on 0.5,
    // so folding a wide horizontal logotype throws it off the quad and stands
    // what is left of it on its side against the two edges: the logo was
    // unreadable on every frame the gate was open. Folding only the field
    // keeps the accent and keeps the type.
    { name: 'logo-colour', cam: false, mono: false, run() {
        const [dr, dg, db] = palette.rgb('deep');
        const [pr, pg, pb] = palette.rgb('purple');
        const [mr, mg, mb] = palette.rgb('magenta');
        const field = () => solid(dr, dg, db, 1)
          .add(noise(2.2, 0.03).color(pr, pg, pb), () => 0.28 + 0.3 * feed.energy)
          .add(osc(9, 0.03, 0).color(mr, mg, mb), 0.2);
        // luma() first to key the black background out (see LOGO_KEY), then
        // mask() to drop the vertical repeats. Both are needed and they do
        // different jobs: the key gives the logo an alpha it does not have,
        // and the tile mask deals with fract(st), which otherwise stacks five
        // copies of the logo up the frame at this scale. Keying alone leaves
        // the repeats, masking alone leaves the black rectangle.
        field()
          .blend(field().kaleid(2), () => 0.12 + 0.5 * feed.swell)
          .modulate(noise(1.3, 0.015), () => 0.02 + 0.05 * feed.swell)
          .layer(src(s2).luma(LOGO_KEY[0], LOGO_KEY[1]).mask(oneTile())
            .scale(() => (0.2 + 0.008 * feed.swell) * kick(KICK_SCALE)(), LOGO_X, 1))
          .out(o0);
    } },

    // 12. Zoom feedback in magenta and violet: two oscillators crossed, the
    // output folded back into its own coordinate. The zoom used to be pumped
    // by the bass at 0.12 a kick, which lurched; it now sits near one and a
    // bit on the energy, with a beat worth under one percent, so the tunnel
    // pulls rather than lurches. Each percent of zoom is ten pixels a frame
    // at the edge of a 960 wide target, which is why the number is small.
    //
    // The feedback depth matters as much as the zoom. Above about 0.2 the
    // coordinate feedback stops being a tunnel and starts reorganising
    // itself: the picture would sit still for a second and then turn itself
    // inside out in three frames, which measured as a seven percent jump out
    // of a one percent baseline. At 0.15 it is a tunnel that keeps moving and
    // never snaps.
    //
    // Every osc() feeding a .color() in this family takes offset 0. osc's
    // third argument phase shifts the three channels against each other, so
    // at offset 1 the source is already full spectrum and .color() only
    // scales what is there: the palette magenta and violet came out as blue
    // and dark red on screen. At offset 0 the channels are equal, the source
    // is grey, and the tint is exactly the palette value.
    { name: 'tunnel', cam: false, mono: false, run() {
        const [r, g, b] = palette.rgb('magenta');
        const [r2, g2, b2] = palette.rgb('violet');
        // The kick is on the ring density, not on the zoom: the zoom is
        // inside the feedback loop and a pop there compounds frame on frame
        // until the tunnel turns itself inside out. An oscillator frequency
        // is read fresh every frame and feeds nothing back.
        osc(() => 14 * kick(KICK_SCALE)(), 0.015, 0).color(r, g, b)
          .blend(osc(() => 22 * kick(KICK_SCALE)(), -0.011, 0).color(r2, g2, b2), 0.5)
          .modulate(src(o0), () => 0.15 + 0.04 * feed.swell)
          .scale(() => 1.002 + 0.006 * feed.energy + 0.003 * feed.pulse)
          .rotate(() => flow() * 0.02)
          .kaleid(3)
          .out(o0);
    } },

    // 13. The drop, as a bloom rather than a strobe, and named bloom-drop for
    // it. As strobe-drop it watched for the bass crossing a threshold and
    // threw a pink and white flash at the screen for 170 ms, rate limited to
    // one every 500 ms: a strobe, on an appliance that sits in front of a DJ
    // for hours.
    //
    // What opens the pink now is the swell, so it comes up over a bar or two
    // as the track builds and falls away again over four, and the beat adds a
    // few percent through a spring on top of that. There is no white in the
    // chain at all, and no discontinuity anywhere in it.
    { name: 'bloom-drop', cam: false, mono: false, run() {
        const [pr, pg, pb] = palette.rgb('plum');
        const [dr, dg, db] = palette.rgb('deep');
        const [kr, kg, kb] = palette.rgb('pink');
        const bloom = () => 0.28 + 0.6 * feed.swell
          + 0.07 * feed.spring('drop-kick', () => feed.pulse, 2.2);
        solid(dr, dg, db, 1)
          .add(noise(2.4, 0.02).color(pr, pg, pb), 0.35)
          .add(osc(16, 0.04, 0).color(kr, kg, kb).kaleid(5)
            .modulate(noise(1.1, 0.015), () => 0.04 + 0.05 * feed.swell)
            .scale(() => (1 + 0.06 * Math.sin(flow() * 0.23)) * kick(KICK_SCALE)()), bloom)
          .out(o0);
    } },

    // 14. Webcam posterised to a few levels and pushed through the palette:
    // the image in magenta, its inverse in violet, mixed back over the
    // previous frame for trails. Five levels, fixed; the bass used to ride
    // the level count between six and three and the whole picture re-banded
    // on every kick. The image is lifted slowly underneath the quantiser
    // instead, which slides the bands rather than rebuilding them, and the
    // swell lengthens the trail.
    { name: 'cam-posterise', cam: true, mono: false, run() {
        const [mr, mg, mb] = palette.rgb('magenta');
        const [vr, vg, vb] = palette.rgb('violet');
        const face = () => src(s0).saturate(0)
          .brightness(() => 0.03 * Math.sin(flow() * 0.4))
          .posterize(5, 1);
        face().color(mr, mg, mb)
          .add(face().invert().color(vr, vg, vb), 0.55)
          .scale(kick(KICK_SCALE))
          .blend(src(o0), () => 0.55 + 0.1 * feed.swell)
          .out(o0);
    } },

    // 15. A scanline field over a palette wash, and a scanline field is what
    // the name says now. As glitch-scan it threw a random block count in on
    // every beat and pixelated the frame with it, easing back out over
    // 420 ms: the single twitchiest thing in the library, and the one the DJ
    // picked out. There is no pixelate here at all now.
    //
    // The scanlines are a soft sine rather than a posterised square, so
    // nothing in the picture is quantised, and their spacing follows the
    // swell: about forty-five lines through a quiet passage closing to
    // seventy-five through a loud one, sliding slowly down the frame the
    // whole time. The beat is a shimmer, five percent on the line brightness
    // and nothing else; it is worth keeping small because this mask
    // multiplies the entire frame, so a lift that would be subtle on a
    // wordmark is a flash of the whole picture here. The drift is a crawl: a
    // scanline field is a comb, and a comb moving a pixel between rendered
    // frames changes every pixel it covers.
    //
    // The hue drift is measured against 174 BPM, the tempo this box is built
    // for, and is zero there. hue() takes turns, not degrees: an absolute
    // 0.0006 * bpm is a sixth of a turn at drum and bass tempo, which swings
    // the palette magenta right round into orange. Referencing the drift to
    // the target tempo keeps a normal set inside the magenta to violet end of
    // the palette and still pulls the colour when the tempo is unusual.
    //
    // feed.bpm is 0, not the current tempo, whenever no deck is the master:
    // at startup, and for the twenty seconds between a stop and the idle
    // fallback. Taken literally that is a quarter turn of hue, straight out of
    // the palette, so a missing tempo falls back to the target instead.
    { name: 'scan-field', cam: false, mono: false, run() {
        const [mr, mg, mb] = palette.rgb('magenta');
        const [vr, vg, vb] = palette.rgb('violet');
        // The spacing goes through a slow spring rather than reading the
        // swell directly. A comb's lines move by their distance from the
        // origin times the change in frequency, so the far edge of the frame
        // travels a third of a line for a hundredth of swell: the swell's own
        // small ripple over each beat, invisible anywhere else, came out here
        // as a visible twitch of the whole field twice a bar. At 0.3 Hz the
        // spring passes the shape of a build and none of the ripple.
        const lines = () => (45 + 30 * feed.spring('scan-lines', () => feed.swell, 0.3)) * TAU;
        const lit = accent(0.57, 0.02);
        // The only consumer of flow() in the file that counts turns rather
        // than radians, so the rule that every rate is a multiple of 0.001
        // does not make its wrap invisible: a rate has to put a whole number
        // of turns into one period instead. Sixteen of them is 0.00255 a
        // second, the crawl this wants, and the modulo keeps the number small
        // for the float32 uniform. A scroll is sampled through fract(), so a
        // jump of exactly one turn cannot be seen and a jump of 0.7 of one,
        // which is what a round 0.0025 would give at the wrap, would be the
        // whole comb moving at once.
        const drift = 16 / FLOW_PERIOD;
        const scan = () => osc(lines, 0, 0).rotate(Math.PI / 2)
          .scrollY(() => (flow() * drift) % 1)
          .brightness(0.45).contrast(1.4)
          .mult(solid(lit, lit, lit, 1));
        osc(18, 0.02, 0).color(mr, mg, mb)
          .add(noise(3, 0.02).color(vr, vg, vb), 0.4)
          .hue(() => {
            const bpm = feed.bpm > 20 ? feed.bpm : 174;
            return 0.02 * Math.sin(flow() * 0.2) + 0.25 * ((bpm - 174) / 174);
          })
          .modulate(noise(1.2, 0.015), () => 0.02 + 0.05 * feed.swell)
          // The wash kicks and the comb does not: a comb moving on the
          // beat is the twitch this sketch was rebuilt to remove.
          .scale(kick(KICK_SCALE))
          .mult(scan())
          .out(o0);
    } },

    // 16. A plasma of oscillator and noise in the palette, folded six ways.
    // The rotation runs on the shared clock, so a busy passage spins it and a
    // sparse break lets it settle, and the fold scale opens on the swell.
    { name: 'plasma-kaleid', cam: false, mono: false, run() {
        const [mr, mg, mb] = palette.rgb('magenta');
        const [vr, vg, vb] = palette.rgb('violet');
        const [kr, kg, kb] = palette.rgb('pink');
        osc(9, 0.03, 0).color(mr, mg, mb)
          .add(noise(3, 0.04).color(vr, vg, vb), 0.45)
          .add(osc(24, -0.03, 0).thresh(0.7, 0.1).color(kr, kg, kb),
            () => 0.12 + 0.2 * feed.energy)
          .kaleid(6)
          .rotate(() => flow() * 0.11)
          .scale(() => (1 + 0.05 * feed.swell) * kick(KICK_SCALE)())
          .out(o0);
    } },

    // 24. The one pattern sketch in the colour family: any pattern at all,
    // drawn as palette-coloured line work on a dark field rather than as
    // white.
    //
    // The first version multiplied a wash by the pattern and came out nearly
    // black. A wash built by adding palette tokens at a weight peaks well
    // under any one of them, so the brightest line in the frame was about
    // half of the palette magenta and the screenshot looked like an unlit
    // room. The lines are now the palette token itself at full value, lifting
    // from magenta toward pink on the swell, and the field is a separate,
    // deliberately dim thing underneath: the deep token at a fifth, plus a
    // slow violet oscillator at a tenth, which is dark enough that the lines
    // are still the picture.
    //
    // layer(), and luma() to make the alpha it needs. The pattern canvases are
    // opaque black outside the line work, so there is nothing to composite
    // with until luma turns luminance into alpha; after that the colour is
    // laid on at full strength wherever the artwork is lit, instead of being
    // scaled by whatever the field happened to be there.
    //
    // The osc feeding the field takes offset 0 for the reason spelt out at
    // tunnel: at any other offset the three channels are already out of phase,
    // the source is full spectrum before .color() touches it, and the palette
    // violet arrives on screen as blue.
    { name: 'pattern-tint', cam: false, mono: false, pattern: true, run() {
        const p = patterns.take([]);
        if (!p) { patternFallback(); return; }
        window.sketchUpdate = () => patterns.bind(p, patterns.sweepT());
        const [dr, dg, db] = palette.rgb('deep');
        const [vr, vg, vb] = palette.rgb('violet');
        const [mr, mg, mb] = palette.rgb('magenta');
        const [kr, kg, kb] = palette.rgb('pink');
        // Magenta at rest, pink through a loud passage, and a few percent on
        // the beat. Every one of these sits at or above the palette magenta.
        const lift = () => 0.35 * feed.swell + 0.05 * feed.pulse;
        const cr = () => mr + (kr - mr) * lift();
        const cg = () => mg + (kg - mg) * lift();
        const cb = () => mb + (kb - mb) * lift();
        const field = () => solid(dr * 0.2, dg * 0.2, db * 0.2, 1)
          .add(osc(5, 0.012, 0).color(vr, vg, vb).rotate(() => flow() * 0.02), 0.1);
        field()
          .layer(sweepPair(0)
            .scrollX(driftX(0.015, 0.04))
            .scale(() => (1.04 + 0.05 * feed.swell) * kick(KICK_SCALE)(), 1, SQUARE_Y)
            .luma(0.12, 0.08)
            .color(cr, cg, cb))
          .out(o0);
    } }

  ];

  window.sketches = [...monochrome, ...colour];

  // The contour sketch slowed right down, with the wordmark sitting under it
  // at 30 percent alpha. This is what the page shows when no deck has sent a
  // beat for twenty seconds, so nothing here reads feed at all, and nothing
  // here reads flow() either: an idle screen should drift at one rate
  // whatever the feed last said.
  window.idleSketch = { name: 'idle-contours', cam: false, mono: true, run() {
      noise(2, 0.008).modulate(noise(0.9, 0.004), 0.12).posterize(9, 1).out(o1);
      edges(() => src(o1), 3)
        .layer(wordmark(0.32).mult(solid(1, 1, 1, 0.3)))
        .out(o0);
  } };
})();
