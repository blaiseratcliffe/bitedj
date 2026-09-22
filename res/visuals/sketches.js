// The sketch library for the HDMI visuals page: sixteen rotation sketches and
// the idle fallback.
//
// The contract with director.js, which owns the rotation:
//   - the director blanks o0..o3 and calls render(o0) before it runs a
//     sketch, so every chain here ends in .out(o0). o1..o3 are free scratch
//     for a sketch that wants a cheap intermediate pass, and several below
//     use o1 to build a field once instead of four times.
//   - it clears the sketch-scoped beat listeners and sets window.sketchUpdate
//     back to null on every switch, so run() is the right and the only place
//     to call feed.onBeat(fn) or to assign window.sketchUpdate = (dt) => {}.
//     dt is milliseconds.
//   - window.update and feed.onBeatAlways belong to the director. Nothing
//     here touches either, and nothing here calls hush(), which would clear
//     the sources as well as the outputs.
//
// Two families. mono is white line work on pure black: no .color() beyond
// white or a grey scale factor, no fills. colour uses palette.js tokens only.
// Motion comes from the music, not from time alone: bass for size and
// displacement, beats for kicks and cuts, high for sparkle.
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
// the end of the chain, and a warp in source space goes near the front.
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

  // Edges of a source: the difference against copies of itself shifted by a
  // pixel in x and y, then a threshold. hydra's custom GLSL cannot sample
  // textures, so this is the whole edge detector.
  //
  // `make` is a factory, not a chain: the detector needs four independent
  // copies of the source and hydra transforms mutate in place, so passing one
  // chain would splice the shifts and the diff into the very thing being
  // differenced. `gain` may be a number or a function.
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

  // The previous frame, dimmed and blown up a touch, as the base of a chain
  // that layers new line work over it. Line work carries zero alpha off the
  // line once it has been through luma() or comes from a transparent source,
  // so layer() keeps the trail visible underneath.
  function trail(fade, grow) {
    return src(o0).scale(grow).mult(solid(fade, fade, fade, 1));
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

  // What counts as a loud kick on feed.bass. Measured, not assumed: over 240
  // frames of the mock 174 BPM feed, feed.bass ran 0.29 to 0.749 and
  // feed.peak 0.28 to 0.596. feed.js normalises each band against a decaying
  // running maximum and then chases it at ATTACK 0.6 a frame, and a drum and
  // bass kick envelope has decayed well before the smoothed value catches up,
  // so the band tops out around three quarters. The two sketches that gate on
  // a loud kick use this rather than the 0.7 and 0.8 of the spec, which would
  // fire once in a blue moon and never respectively.
  const BASS_LOUD = 0.62;

  // osc(freq) is sin(st * freq), so the number of lines across the coordinate
  // is freq / TAU. Every line count below is written as COUNT * TAU, because
  // a bare frequency is impossible to read as a line count and the first
  // versions of the ridge and terrain sketches came out at a quarter of the
  // density they were meant to have.
  const TAU = 2 * Math.PI;

  const monochrome = [

    // 1. The wordmark breathing on the bass, brightness riding the peak, and
    // a faint outward trail so the strokes glow rather than sit flat.
    { name: 'logo-outline', cam: false, mono: true, run() {
        const bright = () => 0.5 + 0.5 * feed.peak;
        trail(0.78, 1.016)
          .layer(wordmark(() => 0.33 + 0.05 * feed.bass)
            .mult(solid(bright, bright, bright, 1)))
          .out(o0);
    } },

    // 2. The logotype mirrored four ways around the centre, letters readable,
    // turning slowly with a kick on every beat and the ring breathing out on
    // the bass.
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
        let angle = 0, kick = 0;
        feed.onBeat(() => { angle += Math.PI / 12; kick = 1; });
        window.sketchUpdate = (dt) => { kick = Math.max(0, kick - dt / 260); };
        wordmark(() => 0.26 + 0.015 * kick).out(o1);
        const arm = (a) => src(o1).scrollY(() => -0.28 - 0.035 * feed.bass).rotate(a);
        solid(0, 0, 0, 0)
          .layer(arm(0))
          .layer(arm(Math.PI / 2))
          .layer(arm(Math.PI))
          .layer(arm(-Math.PI / 2))
          .rotate(() => angle * 0.5 + hydra.synth.time * 0.03)
          .out(o0);
    } },

    // 3. The wordmark sliced into horizontal bands. The modulator is a
    // stepped ramp carried in the red channel only, with green zeroed, so
    // modulate() shifts x per band and leaves y alone; modulate rather than
    // modulateScrollX because the scroll variants fract the coordinate and
    // the wordmark would wrap instead of sliding.
    //
    // A beat throws a fresh offset in and it decays over about 300 ms. At 174
    // BPM a beat is 345 ms, so a pure decay leaves the thing unsliced most of
    // the time and the name stops meaning anything; a small standing stagger
    // under the jump keeps the bands apart between kicks.
    { name: 'logo-sliced', cam: false, mono: true, run() {
        const STAGGER = 0.035;
        let off = 0;
        feed.onBeat(() => { off = 0.16 * (Math.random() * 2 - 1); });
        window.sketchUpdate = (dt) => { off *= Math.pow(0.05, dt / 300); };
        const bands = () => osc(26, 0, 0).rotate(Math.PI / 2)
          .posterize(5, 1).brightness(-0.5).mult(solid(1, 0, 0, 1));
        const bright = () => 0.6 + 0.4 * feed.peak;
        trail(0.55, 1.003)
          .layer(wordmark(0.34)
            .modulate(bands(), () => STAGGER + off)
            .mult(solid(bright, bright, bright, 1)))
          .out(o0);
    } },

    // 4. Joy Division ridges: sixty thin horizontal lines, evenly spaced and
    // mostly flat, with local bumps where the field pushes them up. The bass
    // scales the bump height, so the ridges heave on the kick.
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
        const bumps = () => noise(4.5, 0.06)
          .mult(noise(2.2, 0.05).thresh(0.3, 0.3))
          .mult(solid(0, 1, 0, 1));
        osc(60 * TAU, 0, 0).rotate(Math.PI / 2)
          .modulate(bumps(), () => 0.015 + 0.13 * feed.bass)
          .thresh(0.93, 0.01)
          .out(o0);
    } },

    // 5. A bundle of some forty-five thin lines bending together, laid over a
    // slow feedback smear so the bundle reads as ribbon rather than as fence.
    //
    // Two numbers do all the work. The count: osc(60) is not sixty lines, it
    // is sixty radians, which is nine and a half, and nine fat bands is not a
    // bundle. And the smear: at fade 0.84 and grow 1.005 the copies pile up
    // into fat white bands and the frame goes two thirds white, the opposite
    // of the look, so the half-life is a couple of frames and the growth is
    // almost nothing. The bright line with a grey body behind it is the point.
    { name: 'ribbons', cam: false, mono: true, run() {
        trail(0.45, 1.0008)
          .layer(keyed(osc(45 * TAU, 0.05, 0)
            .modulate(noise(1.4, 0.04), () => 0.06 + 0.2 * feed.bass)
            .rotate(0.35)
            .thresh(0.94, 0.008)))
          .out(o0);
    } },

    // 6. Topographic contours. The posterised noise field is rendered once
    // into o1 and the edge detector then runs on that texture, which costs
    // one noise pair per frame instead of the eight a four-copy chain would
    // need. The field drifts with mid energy; the band gain follows the bass
    // so the contours brighten on the kick.
    { name: 'contours', cam: false, mono: true, run() {
        noise(2.6, 0.05)
          .modulate(noise(1.1, 0.02), () => 0.08 + 0.5 * feed.mid)
          .posterize(10, 1)
          .out(o1);
        edges(() => src(o1), () => 2.5 + 4 * feed.bass).out(o0);
    } },

    // 7. A flow field made entirely of feedback: every frame the previous
    // frame is displaced along a noise vector field and dimmed a little, and
    // sparse bright points are seeded into it, so the points draw their own
    // trajectories as lines with a dot at the head. Beats open the seed
    // threshold for a moment, which puts a burst of new lines in on the kick.
    { name: 'flow-lines', cam: false, mono: true, run() {
        let seed = 0;
        feed.onBeat(() => { seed = 1; });
        window.sketchUpdate = (dt) => { seed = Math.max(0, seed - dt / 400); };
        src(o0)
          .modulate(noise(3.2, 0.08), () => 0.004 + 0.012 * feed.bass)
          .mult(solid(0.93, 0.93, 0.93, 1))
          .layer(keyed(noise(22, 0.3).thresh(() => 0.8 - 0.14 * seed, 0.01)))
          .out(o0);
    } },

    // 8. Wireframe terrain: a forty by twenty-five mesh of single pixel lines
    // warped by a smooth field and compressing toward the horizon.
    //
    // The warp amplitude is the thing to keep small. At 0.22 the coordinate
    // gradient folds over in places and whole cells collapse into white
    // blobs; a tenth of that bends the mesh without ever folding it.
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
        osc(40 * TAU, 0, 0).thresh(0.984, 0.006)
          .add(osc(25 * TAU, 0.012, 0).rotate(Math.PI / 2).thresh(0.984, 0.006))
          .modulate(noise(1.6, 0.04), () => 0.02 + 0.06 * feed.bass)
          .modulateScale(yRamp(), 1.4, 1.0)
          .out(o0);
    } },

    // 9. Webcam edges with a short trail. The gain follows the high band so
    // the outline crackles on hats, and the trail length follows the bass so
    // the ghosting stretches on the kick. The trail does the visible work of
    // the two: the gain feeds a threshold, and an edge that already clears
    // the threshold does not get any brighter for being multiplied harder.
    { name: 'cam-edges', cam: true, mono: true, run() {
        trail(() => 0.55 + 0.35 * feed.bass, 1.006)
          .layer(edges(() => src(s0), () => 3 + 6 * feed.high))
          .out(o0);
    } },

    // 10. Webcam luminance posterised into a handful of bands and reduced to
    // the band edges, a contour map of whoever is in front of the lens.
    // Same o1 trick as contours: posterise once, detect edges on the result.
    // The band count rides the bass, which adds and drops whole contour lines
    // on the kick; driving only the edge gain would be invisible, because the
    // gain feeds a threshold that the edges already clear.
    { name: 'cam-contours', cam: true, mono: true, run() {
        src(s0).saturate(0)
          .posterize(() => 4 + Math.round(5 * feed.bass), 1)
          .out(o1);
        edges(() => src(o1), () => 3 + 4 * feed.mid).out(o0);
    } }

  ];

  const colour = [

    // 11. The boot logo, centred and readable across about eighty percent of
    // the width, bouncing on the bass over a palette field that folds through
    // kaleid(2) while the bass is over BASS_LOUD.
    //
    // The fold is on the field, never on the logo. kaleid returns
    // r * vec2(cos a, sin a), a radial remap centred on 0 rather than on 0.5,
    // so folding a wide horizontal logotype throws it off the quad and stands
    // what is left of it on its side against the two edges: the logo was
    // unreadable on every frame the gate was open. Folding only the field
    // keeps the accent and keeps the type.
    //
    // The gate is BASS_LOUD, not the 0.7 of the spec. Measured on the mock
    // 174 BPM feed, feed.bass runs 0.29 to 0.749: feed.js chases the
    // AGC-normalised value at ATTACK 0.6 per frame while the kick envelope is
    // already decaying, so the smoothed band never gets near its ceiling. A
    // 0.7 gate would open for a frame or two at the very tip of a kick, if at
    // all, and 0.8 would never open. It is a hard cut between two separate
    // chains because kaleid has no value of nSides that is the identity.
    { name: 'logo-colour', cam: false, mono: false, run() {
        const [dr, dg, db] = palette.rgb('deep');
        const [pr, pg, pb] = palette.rgb('purple');
        const [mr, mg, mb] = palette.rgb('magenta');
        const field = () => solid(dr, dg, db, 1)
          .add(noise(2.2, 0.05).color(pr, pg, pb), () => 0.25 + 0.5 * feed.bass)
          .add(osc(9, 0.05, 0).color(mr, mg, mb), 0.2);
        // luma() first to key the black background out (see LOGO_KEY), then
        // mask() to drop the vertical repeats. Both are needed and they do
        // different jobs: the key gives the logo an alpha it does not have,
        // and the tile mask deals with fract(st), which otherwise stacks five
        // copies of the logo up the frame at this scale. Keying alone leaves
        // the repeats, masking alone leaves the black rectangle.
        field()
          .blend(field().kaleid(2), () => (feed.bass > BASS_LOUD ? 1 : 0))
          .layer(src(s2).luma(LOGO_KEY[0], LOGO_KEY[1]).mask(oneTile())
            .scale(() => 0.2 + 0.015 * feed.bass, LOGO_X, 1))
          .out(o0);
    } },

    // 12. Zoom feedback in magenta and violet: two oscillators crossed, the
    // output folded back into its own coordinate, and the zoom pumped by the
    // bass so the tunnel lurches forward on every kick.
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
        osc(14, 0.08, 0).color(r, g, b)
          .blend(osc(22, -0.05, 0).color(r2, g2, b2), 0.5)
          .modulate(src(o0), 0.25)
          .scale(() => 1.03 + 0.12 * feed.bass)
          .kaleid(3)
          .out(o0);
    } },

    // 13. A dark plum field that a pink and white flash tears open when the
    // bass crosses BASS_LOUD (see the note on that constant; the spec says
    // 0.8, which the smoothed band never reaches). The crossing is a rising
    // edge, not a level, and the flash is rate limited to one per 500 ms in a
    // closure, so at 174 BPM, about 345 ms a beat, it fires on alternate
    // kicks rather than on every frame the bass happens to be loud.
    { name: 'strobe-drop', cam: false, mono: false, run() {
        const [pr, pg, pb] = palette.rgb('plum');
        const [dr, dg, db] = palette.rgb('deep');
        const [kr, kg, kb] = palette.rgb('pink');
        const [wr, wg, wb] = palette.rgb('white');
        let lastFlash = -1e9, flash = 0, wasOver = false;
        window.sketchUpdate = (dt) => {
          const now = performance.now();
          const over = feed.bass > BASS_LOUD;
          if (over && !wasOver && now - lastFlash > 500) { lastFlash = now; flash = 1; }
          wasOver = over;
          flash = Math.max(0, flash - dt / 170);
        };
        solid(dr, dg, db, 1)
          .add(noise(2.4, 0.04).color(pr, pg, pb), 0.45)
          .add(osc(6, 0.2, 0).color(kr, kg, kb).kaleid(5), () => flash * 0.9)
          .add(solid(wr, wg, wb, 1), () => flash * flash * 0.5)
          .out(o0);
    } },

    // 14. Webcam posterised to a few levels and pushed through the palette:
    // the image in magenta, its inverse in violet, blended back over the
    // previous frame for trails. The bass rides the level count, six levels
    // at rest down to three on the kick, so the banding coarsens with it.
    { name: 'cam-posterise', cam: true, mono: false, run() {
        const [mr, mg, mb] = palette.rgb('magenta');
        const [vr, vg, vb] = palette.rgb('violet');
        const bins = () => 3 + Math.round(3 * (1 - feed.bass));
        src(s0).saturate(0).posterize(bins, 1).color(mr, mg, mb)
          .add(src(s0).saturate(0).posterize(bins, 1).invert().color(vr, vg, vb), 0.55)
          .blend(src(o0), 0.45)
          .out(o0);
    } },

    // 15. Scanlines over a pixelated palette field. The block size is thrown
    // small on every beat and eases back out between them.
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
    { name: 'glitch-scan', cam: false, mono: false, run() {
        const [mr, mg, mb] = palette.rgb('magenta');
        const [vr, vg, vb] = palette.rgb('violet');
        let blocks = 160;
        feed.onBeat(() => { blocks = 10 + Math.floor(Math.random() * 26); });
        window.sketchUpdate = (dt) => { blocks += (160 - blocks) * Math.min(1, dt / 420); };
        osc(18, 0.05, 0).color(mr, mg, mb)
          .add(noise(3, 0.1).color(vr, vg, vb), 0.4)
          .hue(() => {
            const bpm = feed.bpm > 20 ? feed.bpm : 174;
            return 0.02 * Math.sin(hydra.synth.time * 0.2) + 0.25 * ((bpm - 174) / 174);
          })
          .pixelate(() => blocks, () => Math.max(6, blocks * 0.56))
          .mult(osc(180, 0, 0).rotate(Math.PI / 2).posterize(2, 1).brightness(0.35))
          .out(o0);
    } },

    // 16. A plasma of oscillator and noise in the palette, folded six ways.
    // The rotation is integrated from mid energy rather than from time, so a
    // busy mid range spins it and a sparse break lets it settle.
    { name: 'plasma-kaleid', cam: false, mono: false, run() {
        const [mr, mg, mb] = palette.rgb('magenta');
        const [vr, vg, vb] = palette.rgb('violet');
        const [kr, kg, kb] = palette.rgb('pink');
        let rot = 0;
        window.sketchUpdate = (dt) => { rot += (dt / 1000) * (0.05 + 0.9 * feed.mid); };
        osc(9, 0.06, 0).color(mr, mg, mb)
          .add(noise(3, 0.12).color(vr, vg, vb), 0.45)
          .add(osc(24, -0.1, 0).thresh(0.7, 0.1).color(kr, kg, kb), () => 0.1 + 0.35 * feed.high)
          .kaleid(6)
          .rotate(() => rot)
          .scale(() => 1 + 0.18 * feed.bass)
          .out(o0);
    } }

  ];

  window.sketches = [...monochrome, ...colour];

  // The contour sketch slowed right down, with the wordmark sitting under it
  // at 30 percent alpha. This is what the page shows when no deck has sent a
  // beat for twenty seconds, so nothing here reads feed at all.
  window.idleSketch = { name: 'idle-contours', cam: false, mono: true, run() {
      noise(2, 0.012).modulate(noise(0.9, 0.006), 0.12).posterize(9, 1).out(o1);
      edges(() => src(o1), 3)
        .layer(wordmark(0.32).mult(solid(1, 1, 1, 0.3)))
        .out(o0);
  } };
})();
