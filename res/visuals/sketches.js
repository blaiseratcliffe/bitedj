// Filled in by the sketch task. Two entries so the director has something to
// rotate between while the page is developed.
window.idleSketch = {
  name: 'idle-contours',
  run() {
    noise(2, 0.05).posterize(8, 1).pixelate(960, 540)
      .diff(noise(2, 0.05).posterize(8, 1).scrollX(0.002))
      .thresh(0.05, 0.01).mult(solid(1, 1, 1, 0.4)).out(o0);
  }
};
window.sketches = [
  { name: 'ridge-lines', cam: false, mono: true, run() {
      osc(60, 0, 0).thresh(0.9, 0.02).rotate(Math.PI / 2)
        .modulate(noise(3, 0.1), () => 0.05 + 0.3 * feed.bass).out(o0);
  } },
  { name: 'tunnel', cam: false, mono: false, run() {
      const [r, g, b] = palette.rgb('magenta');
      osc(8, 0.1, 1.2).color(r, g, b).modulate(o0, 0.3)
        .scale(() => 1.02 + 0.1 * feed.bass).kaleid(2).out(o0);
  } }
];
