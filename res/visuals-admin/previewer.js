// Live previews. Tapping a card or a row opens the HDMI page itself in a
// 16:9 frame, served by the admin service at /visuals/index.html on the
// mock 174 BPM feed in preview and embed mode (plan section 1.4), so what
// plays is exactly the code the TV runs. The TV is not touched.
//
// A pattern plays in the first pattern sketch and a clip in the first video
// sketch, in sketch-list order. Closing the overlay removes the frame, which
// is what stops its WebGL context; a hidden frame would keep drawing.
//
// The embedded page also compares window.sketchList with window.sketches and
// posts the result here (bitedj-sketch-check); any mismatch goes up as a
// banner, since a wrong list means sets naming sketches that do not exist.
// So that someone who never opens a preview still sees it, a hidden frame
// runs the check once per browser session and is removed as soon as it
// answers: a frame left running would keep Hydra drawing on the phone.
(function () {
  const { h } = admin;
  const CHECK_KEY = 'bitedj.admin.sketchCheck';
  const CHECK_TIMEOUT_MS = 20000;
  let open = null;   // { overlay, restoreFocus, onKey, onHashChange, inertEls, frame }
  let checkFrame = null, checkTimer = null;

  function endCheck() {
    clearTimeout(checkTimer);
    checkTimer = null;
    if (checkFrame) { checkFrame.remove(); checkFrame = null; }
  }

  function runHiddenCheck() {
    try { if (sessionStorage.getItem(CHECK_KEY)) return; } catch (e) { /* storage blocked: check anyway */ }
    checkFrame = h('iframe', {
      src: '/visuals/index.html?mock=1&preview=1&embed=1', title: 'Sketch list check', 'aria-hidden': 'true',
      tabindex: '-1', style: { position: 'fixed', left: '-10px', top: '-10px', width: '1px', height: '1px', border: '0', opacity: '0' }
    });
    document.body.appendChild(checkFrame);
    checkTimer = setTimeout(endCheck, CHECK_TIMEOUT_MS);
  }

  function firstWith(lib, flag) {
    const s = lib.sketches.find((x) => x.flags.indexOf(flag) >= 0);
    return s ? s.name : null;
  }

  function close() {
    if (!open) return;
    window.removeEventListener('keydown', open.onKey, true);
    window.removeEventListener('hashchange', open.onHashChange);
    open.inertEls.forEach((el) => el.removeAttribute('inert'));
    open.overlay.remove();
    if (open.restoreFocus && open.restoreFocus.focus) open.restoreFocus.focus();
    open = null;
  }

  async function preview(item) {
    // Captured before the await and before close() below moves focus back to
    // whatever opened the previous preview (a re-entrant preview() while one
    // is already open, only reachable by keyboard past the focus trap below,
    // must record its own opener, not the first preview's).
    const opener = document.activeElement;
    const lib = await admin.library();
    const q = new URLSearchParams({ mock: '1', preview: '1', embed: '1' });
    let title;
    if (item.kind === 'pattern') {
      const s = firstWith(lib, 'pattern');
      if (s) q.set('sketch', s);
      q.set('pattern', item.key);
      title = item.key + (s ? ' in ' + s : '');
    } else if (item.kind === 'clip') {
      const s = firstWith(lib, 'video');
      if (s) q.set('sketch', s);
      q.set('clip', item.key);
      title = item.key + (s ? ' in ' + s : '');
    } else if (item.kind === 'entry') {
      q.set('sketch', item.sketch);
      if (item.pattern) q.set('pattern', item.pattern);
      if (item.clip) q.set('clip', item.clip);
      title = [item.sketch, item.pattern, item.clip].filter(Boolean).join(' · ');
    } else {
      q.set('sketch', item.key);
      title = item.key;
    }
    close();
    // tabindex="-1": the frame plays a preview, it has nothing to tab into
    // (embed=1 hides its own overlay), and a same-origin child frame's own
    // keydown events are dispatched in its own document, invisible to this
    // window's capturing listener below, so once Tab moved focus inside it
    // the wrap-around a few lines down could not see the next Tab press to
    // undo it. Kept out of the tab order, Close is the overlay's only stop.
    const frame = h('iframe', { src: '/visuals/index.html?' + q.toString(), title: 'Preview of ' + title, allow: 'autoplay', tabindex: '-1' });
    const closeButton = h('button', { type: 'button', text: 'Close', onclick: close });
    const overlay = h('div', { class: 'overlay', role: 'dialog', 'aria-modal': 'true', 'aria-label': 'Preview' },
      h('div', { class: 'preview-box' },
        h('div', { class: 'preview-frame' }, frame),
        h('div', { class: 'preview-bar' }, h('div', { class: 'title', text: title }), closeButton),
        h('p', { class: 'preview-note', text: 'Plays on a made-up 174 BPM feed. A phone\'s GPU is weaker '
          + 'than the Pi\'s, so a preview can stutter here when the TV would not. Camera sketches show '
          + 'the recorded stand-in clip, or a test pattern if none was recorded.' })));
    overlay.addEventListener('click', (e) => { if (e.target === overlay) close(); });
    // `inert` on the rest of the page (below) keeps Tab from reaching a card
    // behind the overlay, but with nothing tabbable after the last element
    // inside it a plain Tab has nowhere left to go and drops focus onto
    // <body>, which is outside the overlay; wrap it back to the first (and
    // Shift+Tab from the first back to the last) so focus always stays on
    // one of the overlay's own tabbable controls.
    function focusables() {
      return Array.from(overlay.querySelectorAll('button, [href], input, select, textarea, [tabindex]'))
        .filter((el) => !el.disabled && el.tabIndex >= 0);
    }
    const onKey = (e) => {
      if (e.key === 'Escape') { e.preventDefault(); close(); return; }
      if (e.key !== 'Tab') return;
      const els = focusables();
      if (!els.length) return;
      const first = els[0], last = els[els.length - 1];
      if (e.shiftKey && document.activeElement === first) { e.preventDefault(); last.focus(); }
      else if (!e.shiftKey && document.activeElement === last) { e.preventDefault(); first.focus(); }
    };
    window.addEventListener('keydown', onKey, true);
    // A route change (browser back/forward fires hashchange even though the
    // overlay blocks clicks) must not leave the iframe's WebGL context, and
    // possibly the camera stand-in video, running under whatever view loads
    // next; close() tears the frame down and this listener with it.
    const onHashChange = () => close();
    window.addEventListener('hashchange', onHashChange);
    // aria-modal="true" is a lie unless Tab is actually trapped: make every
    // other top-level element inert so neither the mouse (already blocked by
    // the overlay's full-screen click-catcher) nor the keyboard can reach a
    // card behind it and re-enter preview() while one is already open.
    const inertEls = Array.from(document.body.children);
    inertEls.forEach((el) => el.setAttribute('inert', ''));
    open = { overlay, restoreFocus: opener, onKey, onHashChange, inertEls, frame };
    document.body.appendChild(overlay);
    closeButton.focus();
  }

  window.addEventListener('message', (e) => {
    if (e.origin !== location.origin) return;
    // Same-origin is not the same as "from previewer.js's own frame": only
    // react to a post from the hidden check frame or the open preview frame.
    const fromCheck = checkFrame && e.source === checkFrame.contentWindow;
    const fromPreview = open && e.source === open.frame.contentWindow;
    if (!fromCheck && !fromPreview) return;
    const d = e.data;
    if (!d || d.type !== 'bitedj-sketch-check' || !Array.isArray(d.mismatches)) return;
    admin.setBanner('sketch-check', d.mismatches.length
      ? 'The sketch list in sketches.js does not match the sketches the page runs, so a set may name '
        + 'sketches wrongly: ' + d.mismatches.join('; ') + '.'
      : null);
    if (fromCheck) {
      endCheck();
      try { sessionStorage.setItem(CHECK_KEY, '1'); } catch (err) { /* runs again next load */ }
    }
  });

  window.addEventListener('load', () => setTimeout(runHiddenCheck, 2000));

  admin.preview = preview;
})();
