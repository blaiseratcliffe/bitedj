// The Sequence editor: an ordered list of entries. Each is a sketch (with
// the exact pattern or clip a pattern or video sketch needs), an Intro
// switch, and an optional length in bars; blank means the Switch every knob.
// Intro entries play once when the set starts and the rest loops; a dashed
// line marks where the loop starts. Repeats are allowed: the same sketch can
// appear as often as wanted.
//
// Intro entries must come first (the service rejects anything else). The
// editor keeps that true by construction: Intro on moves an entry to the end
// of the intro block, Intro off moves it to the start of the loop, and a
// dragged entry dropped above the line becomes an intro, below it a looping
// entry.
//
// Dragging is Pointer Events on the handle only (touch-action: none there),
// so it works for a finger and a mouse alike; HTML5 drag and drop does not
// fire for touch on phones. The handle also answers ArrowUp and ArrowDown.
(function () {
  const { h } = admin;
  const GAP = 8;   // .entries gap in admin.css

  admin.registerView('sequence', { mount });

  function mount(root, params) {
    let alive = true, dirty = false, lib = null;
    // The one control to refocus after the next drawEntries() rebuild, so a
    // keyboard or screen-reader user's place in the list survives a redraw.
    // { type: 'entry', entry, kind } names a surviving entry by object
    // identity (drawEntries() rebuilds every row, so an index alone would
    // point at the wrong row once entries move or one is removed) and which
    // of its controls to refocus; { type: 'removed', index } is Remove's own
    // case, where the acted-on entry is gone: the entry that slid into its
    // old index gets focus instead, or Add entry if none did.
    let pendingFocus = null, addButton = null;
    let flagsBySketch = new Map();
    // Above the return below, like every const and let here: one declared
    // after it is never initialised, and reading it throws.
    const flags = (name) => flagsBySketch.get(name) || [];
    let sheet = null;       // the add-entry sheet while it is open
    const model = { name: '', knobs: Object.assign({}, admin.knobDefaults), entries: [] };
    const list = h('ol', { class: 'entries' });
    const bar = admin.checkBar(toSet,
      (r) => (r.ok ? 'Ready to save' : 'Cannot save yet')
        + ' · ' + model.entries.length + (model.entries.length === 1 ? ' entry' : ' entries'),
      save);

    root.append(h('p', { class: 'hint', text: 'Loading the library' }));
    admin.library().then((l) => {
      if (!alive) return;
      lib = l;
      flagsBySketch = new Map(lib.sketches.map((s) => [s.name, s.flags]));
      init();
      render();
      document.body.appendChild(bar.el);
      bar.schedule();
    }, (e) => {
      if (alive) root.replaceChildren(h('p', { class: 'hint', text: 'Could not load the library: ' + admin.describeError(e) }));
    });
    // A conversion finished while this editor was open: the clip pickers
    // gain the new clip before anything can be saved (see app.js).
    const stopListening = admin.onLibrary((l) => {
      if (!alive || !lib) return;
      lib = l;
      flagsBySketch = new Map(lib.sketches.map((s) => [s.name, s.flags]));
      drawEntries();
      bar.schedule();
    });

    return {
      unmount() { alive = false; stopListening(); bar.stop(); bar.el.remove(); closeSheet(); },
      leaving() { return dirty ? 'This sequence has unsaved changes. Leave without saving?' : null; }
    };

    function init() {
      const s = params.set;
      if (s) {
        model.name = s.name;
        model.knobs = Object.assign({}, admin.knobDefaults, s.knobs || {});
        model.entries = (s.entries || []).map((e) => Object.assign({}, e));
      } else {
        model.name = 'New sequence';
        dirty = true;
      }
    }

    function clean(e) {
      const out = { sketch: e.sketch };
      if (e.intro) out.intro = true;
      if (e.bars !== undefined && e.bars !== null && e.bars !== '') out.bars = e.bars;
      // A hand-edited file can leave a pattern or clip on an entry whose
      // sketch does not carry the matching flag (or never did); the sketch
      // picker's own onChange strips this the moment the user visits it, so
      // do the same here rather than send, and report a problem for, a
      // field with no visible control to clear it from.
      const f = flags(e.sketch);
      if (e.pattern && f.indexOf('pattern') >= 0) out.pattern = e.pattern;
      if (e.clip && f.indexOf('video') >= 0) out.clip = e.clip;
      return out;
    }

    function toSet() {
      return { name: model.name.trim(), type: 'sequence', entries: model.entries.map(clean), knobs: Object.assign({}, model.knobs) };
    }

    function changed(redraw) {
      dirty = true;
      if (redraw) drawEntries();
      bar.schedule();
    }

    function render() {
      addButton = h('button', { type: 'button', class: 'primary', text: 'Add entry', onclick: openSheet });
      const name = h('input', { type: 'text', maxlength: 40, value: model.name, autocomplete: 'off', 'aria-label': 'Sequence name' });
      name.addEventListener('input', () => { model.name = name.value; changed(false); });
      root.replaceChildren(
        h('h1', { text: params.set ? 'Edit sequence' : 'New sequence' }),
        h('label', { class: 'field' }, h('span', { text: 'Name' }), name),
        admin.knobRow(model.knobs, () => changed(true)),   // redraw: the Bars placeholders show the knob
        h('h2', { text: 'Running order' }),
        h('p', { class: 'hint', text: 'Intro entries play once when the set starts; everything below '
          + 'the dashed line loops. Blank bars means the Switch every knob.' }),
        list,
        h('div', { class: 'toolbar' }, addButton));
      drawEntries();
    }

    function select(options, value, onChange, label) {
      const s = h('select', { 'aria-label': label });
      for (const [v, t] of options) {
        const o = h('option', { value: v, text: t });
        if (v === value) o.selected = true;
        s.appendChild(o);
      }
      s.addEventListener('change', () => onChange(s.value));
      return s;
    }

    function drawEntries() {
      const items = [];
      const introCount = model.entries.filter((e) => e.intro).length;
      model.entries.forEach((e, i) => {
        if (!e.intro && introCount > 0 && (i === 0 || model.entries[i - 1].intro)) {
          items.push(h('li', { class: 'loop-line', 'aria-hidden': 'true', text: 'Loop starts here' }));
        }
        items.push(entryRow(e, i));
      });
      if (!model.entries.length) items.push(h('li', { class: 'empty', text: 'No entries yet. Add one below.' }));
      list.replaceChildren(...items);
      restorePendingFocus();
    }

    // Records which control to refocus once the rebuild drawEntries() is
    // about to do lands; drawEntries() consumes and clears it every call, so
    // one with nothing pending is a no-op, and every redraw path (Intro,
    // a sketch change, Remove, the arrow keys, a drag, even the plain
    // onLibrary refresh) goes through this one place.
    function focusAfterRedraw(target) { pendingFocus = target; }

    function restorePendingFocus() {
      const target = pendingFocus;
      pendingFocus = null;
      if (!target) return;
      const rows = list.querySelectorAll('li.entry');
      if (target.type === 'removed') {
        if (target.index < rows.length) focusWithin(rows[target.index], 'handle');
        else if (addButton) addButton.focus();
        return;
      }
      const idx = model.entries.indexOf(target.entry);
      if (idx < 0 || idx >= rows.length) return;
      focusWithin(rows[idx], target.kind);
    }

    function focusWithin(row, kind) {
      let el;
      switch (kind) {
        case 'sketch': el = row.querySelector('select[aria-label^="Sketch for entry"]'); break;
        case 'item': el = row.querySelector('select[aria-label^="Pattern for entry"], select[aria-label^="Clip for entry"]'); break;
        case 'intro': el = row.querySelector('input[type=checkbox]'); break;
        case 'bars': el = row.querySelector('.bars'); break;
        default: el = row.querySelector('.handle');
      }
      (el || row.querySelector('.handle')).focus();
    }

    function entryRow(e, i) {
      const f = flags(e.sketch);
      const row = h('li', { class: 'entry' + (e.intro ? ' intro' : '') });
      const handle = h('button', { type: 'button', class: 'handle', 'aria-label': 'Move entry ' + (i + 1) + ', use the arrow keys', text: '≡' });
      handle.addEventListener('pointerdown', (ev) => startDrag(ev, i, row));
      handle.addEventListener('keydown', (ev) => {
        if (ev.key === 'ArrowUp' && i > 0) { ev.preventDefault(); focusAfterRedraw({ type: 'entry', entry: e, kind: 'handle' }); moveTo(i, i - 1); }
        if (ev.key === 'ArrowDown' && i < model.entries.length - 1) { ev.preventDefault(); focusAfterRedraw({ type: 'entry', entry: e, kind: 'handle' }); moveTo(i, i + 1); }
      });

      const sketchOptions = lib.sketches.map((s) => [s.name, s.name + (s.new ? ' (new)' : '')]);
      if (!flagsBySketch.has(e.sketch)) sketchOptions.unshift([e.sketch, e.sketch + ' (not on the Pi)']);
      const sketchSel = select(sketchOptions, e.sketch, (v) => {
        e.sketch = v;
        const nf = flags(v);
        if (nf.indexOf('pattern') < 0) delete e.pattern;
        if (nf.indexOf('video') < 0) delete e.clip;
        focusAfterRedraw({ type: 'entry', entry: e, kind: 'sketch' });
        changed(true);
      }, 'Sketch for entry ' + (i + 1));

      let itemSel = null;
      if (f.indexOf('pattern') >= 0) {
        // Only patterns the page can draw; an entry that already names one it
        // cannot keeps it in the list, marked, so the check bar can say why.
        const pats = lib.patterns.filter((p) => p.playable !== false || p.slug === e.pattern)
          .map((p) => [p.slug, (p.title ? p.title + ' (' + p.slug + ')' : p.slug)
            + (p.playable === false ? ' (will not play: ' + (p.why || 'cannot be drawn') + ')' : '')]);
        // A pattern the library does not list at all (a too-solid one in a
        // hand-edited file, Decision 34) still shows as what the entry names,
        // as the sketch picker does above, rather than "Choose a pattern".
        if (e.pattern && !lib.patterns.some((p) => p.slug === e.pattern)) {
          pats.unshift([e.pattern, e.pattern + ' (not on the Pi)']);
        }
        itemSel = select([['', 'Choose a pattern']].concat(pats),
          e.pattern || '', (v) => { if (v) e.pattern = v; else delete e.pattern; changed(false); }, 'Pattern for entry ' + (i + 1));
      } else if (f.indexOf('video') >= 0) {
        const clips = lib.clips.map((c) => [c.file, c.file]);
        if (e.clip && !lib.clips.some((c) => c.file === e.clip)) {
          clips.unshift([e.clip, e.clip + ' (not on the Pi)']);
        }
        itemSel = select([['', 'Choose a clip']].concat(clips),
          e.clip || '', (v) => { if (v) e.clip = v; else delete e.clip; changed(false); }, 'Clip for entry ' + (i + 1));
      }

      const intro = h('input', { type: 'checkbox', 'aria-label': 'Intro, plays once at the start' });
      intro.checked = !!e.intro;
      intro.addEventListener('change', () => { focusAfterRedraw({ type: 'entry', entry: e, kind: 'intro' }); toggleIntro(i); });

      const bars = h('input', { type: 'number', class: 'bars', min: 1, max: 256, step: 1, inputmode: 'numeric',
        placeholder: String(model.knobs.bars), 'aria-label': 'Bars for entry ' + (i + 1) });
      if (e.bars !== undefined && e.bars !== null) bars.value = String(e.bars);
      bars.addEventListener('input', () => {
        const v = bars.value.trim();
        if (v === '') delete e.bars; else e.bars = Number(v);
        changed(false);
      });

      const canPreview = typeof admin.preview === 'function';
      row.append(handle, h('div', { class: 'fields' },
        h('div', { class: 'line' }, h('span', { class: 'pos', text: (i + 1) + '.' }), sketchSel),
        itemSel ? h('div', { class: 'line' }, itemSel) : null,
        h('div', { class: 'line' },
          h('label', { class: 'intro-toggle' }, h('span', { class: 'switch' }, intro, h('span', { class: 'track' })), 'Intro'),
          bars, h('span', { class: 'hint', text: 'bars' }),
          canPreview ? h('button', { type: 'button', text: 'Preview',
            onclick: () => admin.preview({ kind: 'entry', sketch: e.sketch, pattern: e.pattern || null, clip: e.clip || null }) }) : null,
          h('button', { type: 'button', class: 'danger', 'aria-label': 'Remove entry ' + (i + 1), text: 'Remove',
            onclick: () => { focusAfterRedraw({ type: 'removed', index: i }); model.entries.splice(i, 1); changed(true); } }))));
      return row;
    }

    // Moves entry `from` to index `to` (an index in the list without it),
    // then sets its Intro flag from where it landed: above the intro block's
    // end it is an intro, below the loop's start it loops, and exactly on the
    // boundary it keeps what it had.
    function moveTo(from, to) {
      const [e] = model.entries.splice(from, 1);
      const boundary = model.entries.filter((x) => x.intro).length;
      if (to < boundary) e.intro = true;
      else if (to > boundary) e.intro = false;
      model.entries.splice(to, 0, e);
      changed(true);
    }

    function toggleIntro(i) {
      const [e] = model.entries.splice(i, 1);
      e.intro = !e.intro;
      if (!e.intro) delete e.intro;
      const at = model.entries.filter((x) => x.intro).length;   // end of intros = start of loop
      model.entries.splice(at, 0, e);
      changed(true);
    }

    function startDrag(ev, index, row) {
      if (ev.button !== 0) return;
      ev.preventDefault();
      const handle = ev.currentTarget;
      handle.setPointerCapture(ev.pointerId);
      // By identity, not index: index is only where this entry started,
      // and moveTo() below can leave it somewhere else by the time the
      // drag ends.
      const entry = model.entries[index];
      const rows = Array.from(list.querySelectorAll('li.entry'));
      const rects = rows.map((r) => r.getBoundingClientRect());
      const step = rects[index].height + GAP;
      const startY = ev.clientY;
      let target = index;
      row.classList.add('dragging');
      list.classList.add('sorting');

      function move(e2) {
        const dy = e2.clientY - startY;
        row.style.transform = 'translateY(' + dy + 'px)';
        const y = rects[index].top + rects[index].height / 2 + dy;
        let t = 0;
        rects.forEach((r, k) => { if (k !== index && y > r.top + r.height / 2) t += 1; });
        target = t;
        rows.forEach((r, k) => {
          if (k === index) return;
          const kk = k < index ? k : k - 1;   // its index once the dragged row is out
          const shift = k < index && kk >= target ? step : k > index && kk < target ? -step : 0;
          r.style.transform = shift ? 'translateY(' + shift + 'px)' : '';
        });
      }
      function end(commit) {
        handle.removeEventListener('pointermove', move);
        handle.removeEventListener('pointerup', onUp);
        handle.removeEventListener('pointercancel', onCancel);
        rows.forEach((r) => { r.style.transform = ''; });
        row.classList.remove('dragging');
        list.classList.remove('sorting');
        // preventDefault() above (needed to stop text selection and page
        // scroll while dragging) also blocks the browser's normal
        // click-to-focus, so without this the handle a mouse or finger
        // just used is never focused: drop, cancel or a plain click alike.
        // A real move re-renders the row (this element is gone), so that
        // case waits for the redraw and finds the entry by identity;
        // anything else re-focuses the same handle directly.
        if (commit && target !== index) {
          focusAfterRedraw({ type: 'entry', entry, kind: 'handle' });
          moveTo(index, target);
        } else {
          handle.focus();
        }
      }
      const onUp = () => end(true);
      const onCancel = () => end(false);
      handle.addEventListener('pointermove', move);
      handle.addEventListener('pointerup', onUp);
      handle.addEventListener('pointercancel', onCancel);
    }

    function closeSheet() {
      if (sheet) { sheet.remove(); sheet = null; }
    }
    function openSheet() {
      closeSheet();
      const add = (name) => {
        model.entries.push({ sketch: name });
        closeSheet();
        changed(true);
        const rows = list.querySelectorAll('li.entry');
        if (rows.length) rows[rows.length - 1].scrollIntoView({ block: 'nearest' });
      };
      const tag = (s) => (s.flags.indexOf('pattern') >= 0 ? ' (pattern)' : s.flags.indexOf('video') >= 0 ? ' (clip)'
        : s.flags.indexOf('cam') >= 0 ? ' (camera)' : '');
      sheet = h('div', { class: 'overlay', role: 'dialog', 'aria-modal': 'true', 'aria-label': 'Add an entry' },
        h('div', { class: 'sheet' },
          h('div', { class: 'sheet-head' }, h('h2', { text: 'Add an entry' }), h('button', { type: 'button', text: 'Close', onclick: closeSheet })),
          h('div', { class: 'sheet-list' }, ...lib.sketches.map((s) =>
            h('button', { type: 'button', text: s.name + tag(s), onclick: () => add(s.name) })))));
      sheet.addEventListener('click', (e) => { if (e.target === sheet) closeSheet(); });
      document.body.appendChild(sheet);
    }

    // A15's save step, shared with the Random editor rather than copied.
    // toSet goes in as getSet, so an edit made while the save is in flight
    // stays on screen and unsaved rather than being reloaded away.
    function save() {
      return admin.saveSetAndGo(params, toSet(), bar, () => { dirty = false; }, toSet);
    }
  }
})();
