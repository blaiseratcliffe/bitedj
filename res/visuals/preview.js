// Preview mode: an overlay for stepping through every sketch and every Book
// of Shapes pattern from a desktop browser, with their names on screen.
//
// Off unless the URL carries ?preview=1, and this file returns on its first
// line otherwise: no overlay, no key handler, no pause, no pin. That matters
// on the Pi, where a keystroke meant for the app can land in Chromium (a
// typed `d` went there once after a visuals launch), so there must be no key
// handling at all outside this mode.
//
// Serve res/visuals with `python -m http.server 8765 --bind 127.0.0.1` and
// open index.html?mock=1&preview=1; ?mock=1 is feed.js's synthetic 174 BPM
// feed. Without ?mock=1 it reads the real feed, for instance the Pi's through
// `ssh -L 7374:127.0.0.1:7374 bitepi`.
//
// What it does, through the hooks director.js and patterns.js expose for it:
// the rotation is paused, so a sketch stays until you move; the sketch
// buttons step window.sketches in list order, including the sketches the
// rotation would skip, except a camera sketch with no camera; Next is the
// director's own pickNext(); and choosing a pattern pins it, so every pattern
// sketch draws it until `auto`. A desktop is several times faster than the
// Pi, so nothing seen here says how the Pi copes with a pattern.
(function () {
  if (new URLSearchParams(location.search).get('preview') !== '1') return;
  const director = window.director;
  if (!director) {
    console.error('visuals: preview mode needs window.director from director.js');
    return;
  }
  const pats = window.patterns || null;
  const NOTE_MS = 4000;

  director.pause(true);
  // index.html hides the pointer for the TV; a desktop needs it back.
  document.body.style.cursor = 'default';

  const css = document.createElement('style');
  css.textContent = [
    '#preview { position: fixed; left: 12px; bottom: 12px; z-index: 10;',
    '  max-width: calc(100vw - 24px); box-sizing: border-box; padding: 10px 12px;',
    '  background: rgba(0, 0, 0, 0.62); color: #e8e8e8; border-radius: 6px;',
    '  font: 14px/1.45 "DejaVu Sans Mono", Consolas, monospace; cursor: default; }',
    '#preview .label { white-space: pre; font-size: 15px; color: #fff; }',
    '#preview .sub { white-space: pre; color: #b8b8b8; min-height: 1.45em; }',
    '#preview .row { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 8px; align-items: center; }',
    '#preview button, #preview select { font: inherit; color: #eee; background: #222;',
    '  border: 1px solid #555; border-radius: 4px; padding: 3px 8px; cursor: pointer; }',
    '#preview button:hover, #preview select:hover { background: #333; }',
    '#preview select { max-width: 340px; }',
    '#preview .hint { white-space: pre; margin-top: 6px; color: #8c8c8c; font-size: 12px; }'
  ].join('\n');
  document.head.appendChild(css);

  const box = document.createElement('div');
  box.id = 'preview';
  const label = document.createElement('div');
  label.className = 'label';
  const sub = document.createElement('div');
  sub.className = 'sub';
  const row = document.createElement('div');
  row.className = 'row';
  const hint = document.createElement('div');
  hint.className = 'hint';
  hint.textContent = 'keys: \u2190 \u2192 sketch   \u2191 \u2193 pattern   n next   h hide';

  function button(text, fn) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = text;
    b.addEventListener('click', fn);
    row.appendChild(b);
    return b;
  }

  button('\u25C0 sketch', () => stepSketch(-1));
  button('sketch \u25B6', () => stepSketch(1));
  button('Next', next);
  button('\u25C0 pattern', () => stepPattern(-1));
  button('pattern \u25B6', () => stepPattern(1));

  // The pattern list, with `auto` on top. Patterns the rotation leaves out
  // are still here, with the reason, because seeing them is the point.
  const select = document.createElement('select');
  const auto = document.createElement('option');
  auto.value = '';
  auto.textContent = 'auto';
  select.appendChild(auto);
  const options = [];
  (pats ? pats.library() : []).forEach((p) => {
    const o = document.createElement('option');
    o.value = p.slug;
    options.push(o);
    select.appendChild(o);
  });
  select.addEventListener('change', () => {
    choose(select.value || null);
    // Hand the keys back to the page, or the arrows would move the list.
    select.blur();
  });
  row.appendChild(select);

  box.appendChild(label);
  box.appendChild(sub);
  box.appendChild(row);
  box.appendChild(hint);
  document.body.appendChild(box);

  let note = '', noteUntil = 0;
  function flash(text) {
    note = text;
    noteUntil = performance.now() + NOTE_MS;
    render();
  }

  function optionText(p, i) {
    return (i + 1) + '. ' + p.slug + (p.status === 'in rotation' ? '' : ' (' + p.status + ')');
  }

  let shownPin;
  function render() {
    const s = director.current();
    const list = director.list();
    const lib = pats ? pats.library() : [];
    let text = 'starting';
    if (s) {
      const at = list.indexOf(s);
      text = (at >= 0 ? (at + 1) + ' / ' + list.length : 'idle') + '  ' + s.name;
    }
    const loading = pats ? pats.pinLoading() : null;
    if (loading) {
      text += '  \u00B7  loading ' + loading;
    } else if (s && s.pattern && pats) {
      const t = pats.taken();
      text += '  \u00B7  ' + (t.length ? t.join(' + ') : 'no pattern ready yet');
    }
    label.textContent = text;

    const pin = pats ? pats.pinned() : null;
    let line = 'pattern: auto';
    if (pin) {
      const at = lib.findIndex(p => p.slug === pin);
      line = 'pattern: ' + pin + ' pinned, ' + (at + 1) + ' / ' + lib.length
        + (at >= 0 && lib[at].status !== 'in rotation' ? ', ' + lib[at].status : '');
    }
    if (note && performance.now() < noteUntil) line += '   ' + note;
    else note = '';
    sub.textContent = line;

    lib.forEach((p, i) => {
      const want = optionText(p, i);
      if (options[i] && options[i].textContent !== want) options[i].textContent = want;
    });
    if (pin !== shownPin) {
      select.value = pin || '';
      shownPin = pin;
    }
  }

  // Steps from where the show is heading rather than from what is on screen,
  // so two quick steps during a melt move two places.
  function stepSketch(dir) {
    const list = director.list();
    const from = list.indexOf(director.target());
    let i = from < 0 ? (dir > 0 ? -1 : list.length) : from;
    for (let n = 0; n < list.length; n++) {
      i = (i + dir + list.length) % list.length;
      const s = list[i];
      if (s.cam && !window.camReady) {
        flash('no camera, skipped ' + s.name);
        continue;
      }
      director.show(s);
      return;
    }
  }

  function next() {
    director.show(director.pickNext());
  }

  function stepPattern(dir) {
    if (!pats) return;
    const lib = pats.library();
    if (!lib.length) return;
    const slug = pats.pinned() || pats.taken()[0];
    const at = lib.findIndex(p => p.slug === slug);
    const i = at < 0 ? (dir > 0 ? 0 : lib.length - 1) : (at + dir + lib.length) % lib.length;
    choose(lib[i].slug);
  }

  // Pin a pattern, or with null go back to normal picking. Once the pattern
  // is in the cache, a pattern sketch restarts so it takes it, and anything
  // else gives way to the first pattern sketch in list order.
  function choose(slug) {
    if (!pats) return;
    const target = director.target();
    if (!slug) {
      pats.unpin();
      console.log('visuals: preview pattern auto');
      if (target && target.pattern) director.show(target);
      render();
      return;
    }
    if (slug === pats.pinned() && pats.pinLoading()) return;
    const cached = pats.library().some(p => p.slug === slug && p.cached);
    console.log('visuals: preview pin', slug, cached ? 'from the cache' : 'loading');
    pats.pin(slug, (entry) => {
      if (!entry) {
        flash('could not load ' + slug);
        return;
      }
      // A pattern sketch restarts where it is. The jump to the first pattern
      // sketch only happens if nobody moved while the pattern loaded; a step
      // to another sketch in the meantime is where the show stays.
      const s = director.target();
      if (s && s.pattern) {
        const drawing = s === director.current() && pats.taken().indexOf(slug) === 0;
        if (!drawing) director.show(s);
      } else if (s === target) {
        director.show(director.list().find(x => x.pattern));
      }
      render();
    });
    render();
  }

  // Capture phase, so an arrow pressed with the list focused is ours and not
  // the list's. Anything with a modifier is left to the browser, and so are
  // `n` and `h` while the list has focus. A held pattern key does not
  // repeat: every step starts a load, and a burst of them only cancels each
  // other.
  window.addEventListener('keydown', (e) => {
    if (e.ctrlKey || e.altKey || e.metaKey) return;
    if (e.repeat && (e.key === 'ArrowUp' || e.key === 'ArrowDown')) {
      e.preventDefault();   // nor does the list, if it has focus
      return;
    }
    if (document.activeElement === select && /^[nNhH]$/.test(e.key)) return;
    let act = null;
    switch (e.key) {
      case 'ArrowLeft': act = () => stepSketch(-1); break;
      case 'ArrowRight': act = () => stepSketch(1); break;
      case 'ArrowUp': act = () => stepPattern(-1); break;
      case 'ArrowDown': act = () => stepPattern(1); break;
      case 'n': case 'N': act = next; break;
      case 'h': case 'H':
        act = () => { box.style.display = box.style.display === 'none' ? '' : 'none'; };
        break;
      default: return;
    }
    e.preventDefault();
    e.stopPropagation();
    act();
  }, true);

  director.onSwitch(() => render());
  setInterval(render, 500);
  render();
  console.log('visuals: preview mode on, rotation paused');
})();
