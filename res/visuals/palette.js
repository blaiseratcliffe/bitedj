// Colour tokens sampled from the boot logo (pi/assets/boot-logo.png), so the
// colour sketches read as one system with the wordmark.
window.palette = {
  deep: '#240f2d', purple: '#45174e', plum: '#794470', violet: '#9861b8',
  magenta: '#e24992', pink: '#fd9cc2', white: '#ffffff',
  rgb(name) {
    const h = this[name].slice(1);
    return [0, 2, 4].map(i => parseInt(h.slice(i, i + 2), 16) / 255);
  }
};
