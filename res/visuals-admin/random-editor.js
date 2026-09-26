// The Random set editor. A name and the five knobs, then one tab of cards
// for each kind of item. A card's switch ticks the item into the set; its
// picture opens a live preview once previewer.js has defined admin.preview.
// The bar pinned to the bottom asks the service's own validator (POST
// /api/sets/check) how many sketches can play, so the rules exist once, in
// pi/bin/bitedj-visuals-admin, and Save stays disabled while it reports a
// problem.
//
// Also exports the knob row, the check bar and the save step
// (admin.knobRow, admin.checkBar, admin.saveSetAndGo) for
// sequence-editor.js, and admin.formatSeconds for clips.js.
(function () {
  const { h } = admin;

  // The panel's rows and values (settings.xml, Visuals rows 2 to 6).
  const KNOBS = [
    { key: 'bars', label: 'Switch every (bars)', options: [[8, '8'], [16, '16'], [32, '32'], [64, '64']] },
    { key: 'reactivity', label: 'Reactivity', options: [[0, 'Subtle'], [1, 'Medium'], [2, 'Strong']] },
    { key: 'bounce', label: 'Bounce', options: [[0, 'Off'], [1, 'Low'], [2, 'Medium'], [3, 'High']] },
    { key: 'swirl', label: 'Swirl', options: [[0, 'Off'], [1, 'Low'], [2, 'Medium'], [3, 'High']] },
    { key: 'camMix', label: 'Camera mix', options: [[0, 'Off'], [1, 'On']] }
  ];

  function knobRow(knobs, onChange) {
    const box = h('section', { class: 'knobs' },
      h('h2', { text: 'Knobs' }),
      h('p', { class: 'hint', text: 'Picking this set on the panel sets these. '
        + 'Changes made on the panel after that are not saved back here.' }));
    for (const k of KNOBS) {
      const seg = h('div', { class: 'segmented', role: 'group', 'aria-label': k.label });
      for (const [value, text] of k.options) {
        const b = h('button', { type: 'button', class: 'seg', 'aria-pressed': String(knobs[k.key] === value), text });
        b.addEventListener('click', () => {
          knobs[k.key] = value;
          for (const x of seg.children) x.setAttribute('aria-pressed', String(x === b));
          onChange();
        });
        seg.appendChild(b);
      }
      box.appendChild(h('div', { class: 'knob' }, h('span', { class: 'knob-label', text: k.label }), seg));
    }
    return box;
  }

  function checkBar(getSet, headline, onSave) {
    const head = h('div', { class: 'headline', text: 'Checking' });
    const items = h('ul');
    const info = h('div', { class: 'check', 'aria-live': 'polite' }, head, items);
    const save = h('button', { type: 'button', class: 'primary', text: 'Save', disabled: true });
    const el = h('div', { class: 'savebar' }, h('div', { class: 'inner' }, info, save));
    let timer = null, seq = 0, result = null, saving = false, error = null;

    function update() {
      save.disabled = saving || !result || !result.ok;
      save.textContent = saving ? 'Saving' : 'Save';
    }
    function render(r) {
      result = r;
      const problems = r.problems || [];
      const bad = problems.length > 0 || !!error;
      info.classList.toggle('bad', bad);
      info.classList.toggle('good', !bad);
      head.textContent = error ? 'Not saved' : headline(r);
      items.replaceChildren(
        ...(error ? [h('li', { class: 'problem', text: error })] : []),
        ...problems.map((p) => h('li', { class: 'problem', text: p })),
        ...(r.notes || []).map((n) => h('li', { text: n })));
      update();
    }
    async function run() {
      const my = ++seq;
      info.classList.add('checking');
      try {
        const r = await api.check(getSet());
        if (my === seq) render(r);
      } catch (e) {
        if (my === seq) render({ ok: false, playable: 0, problems: [admin.describeError(e)], notes: [] });
      } finally {
        if (my === seq) info.classList.remove('checking');
      }
    }
    save.addEventListener('click', async () => {
      if (save.disabled) return;
      clearTimeout(timer);
      saving = true;
      update();
      try { await onSave(); } finally { saving = false; update(); }
    });
    return {
      el,
      schedule() { error = null; clearTimeout(timer); timer = setTimeout(run, 200); },
      now() { error = null; clearTimeout(timer); return run(); },
      // The service refused the set as invalid: Save stays off until an edit
      // is checked again.
      showProblems(list) {
        seq += 1;   // a check still in flight must not paint over the save's reasons
        error = null;
        render({ ok: false, playable: result ? result.playable : 0, problems: list, notes: [] });
      },
      // Any other failure (the network, a 500): say so, and leave Save as the
      // last check left it, so the same content can be retried.
      showError(text) {
        seq += 1;
        error = text;
        render(result || { ok: false, playable: 0, problems: [], notes: [] });
      },
      stop() { clearTimeout(timer); seq += 1; }
    };
  }

  // Saves `body` as a new set (params.id null) or over set params.id, then
  // opens the saved set. onSaved() runs first, so the editor can clear its
  // unsaved-changes flag before the navigation asks about it. A 400 with
  // reasons means the set is invalid: fix it first. Anything else (the
  // network, a 500): the same content can be saved again. The Sequence
  // editor (sequence-editor.js, Task B3) saves through this too.
  //
  // `getSet` is the editor's own snapshot function (its toSet()), called
  // again once the request comes back, so two races that a plain
  // "always clear dirty, always reload" version got wrong are handled here,
  // once, for every editor that calls this (fix round 1, task A15 review):
  //   - an edit made to the form while the request is still in flight must
  //     not be thrown away. If getSet() no longer matches the `body` that
  //     was sent, the edit stays on screen: onSaved() is not called (the
  //     unsaved-changes flag stays armed) and there is no reload. The new
  //     id is still remembered on `params`, so a create only ever creates
  //     once even though this save turns out to have been stale; the next
  //     Save the user makes PUTs.
  //   - leaving the editor (confirming "leave without saving" from
  //     route()) while the request is still in flight must not drag the
  //     browser back once it resolves. If the hash has moved on by then,
  //     the toast still fires (the save did happen) but nothing navigates.
  async function saveSetAndGo(params, body, bar, onSaved, getSet) {
    const hashAtStart = location.hash;
    try {
      const r = params.id == null ? await api.createSet(body) : await api.saveSet(params.id, body);
      params.id = r.set.id;
      admin.invalidateLibrary();   // a save adds everything shown to `known`
      admin.toast('Saved "' + r.set.name + '"');
      const edited = getSet && JSON.stringify(getSet()) !== JSON.stringify(body);
      const left = location.hash !== hashAtStart;
      if (!edited && !left) {
        onSaved();
        admin.go('#set/' + r.set.id, { reload: true });
      }
    } catch (e) {
      if (e.status === 400 && e.problems && e.problems.length) bar.showProblems(e.problems);
      else bar.showError('Not saved: ' + admin.describeError(e));
    }
  }

  admin.knobRow = knobRow;
  admin.checkBar = checkBar;
  admin.saveSetAndGo = saveSetAndGo;

  const TABS = [
    { key: 'sketches', label: 'Sketches', kind: 'sketch' },
    { key: 'patterns', label: 'Patterns', kind: 'pattern' },
    { key: 'clips', label: 'Clips', kind: 'clip' }
  ];

  // A sketch has no picture of its own, so its card shows its initials on a
  // colour worked out from its name: stable between visits, different
  // between neighbours.
  function tile(name) {
    let hash = 0;
    for (let i = 0; i < name.length; i++) hash = (hash * 31 + name.charCodeAt(i)) >>> 0;
    const initials = name.split(/[-_\s]+/).filter(Boolean).map((w) => w[0]).join('').slice(0, 3).toUpperCase();
    return h('div', { class: 'tile', style: { background: 'hsl(' + (hash % 360) + ' 42% 30%)' }, text: initials || '?' });
  }

  // A clip's length as m:ss, or '' when unknown. Exported as
  // admin.formatSeconds; clips.js (Task A16) uses it rather than its own.
  function formatSeconds(s) {
    if (!(s > 0)) return '';
    const m = Math.floor(s / 60);
    const r = Math.round(s % 60);
    return m + ':' + String(r).padStart(2, '0');
  }
  admin.formatSeconds = formatSeconds;

  const FLAG_TEXT = { cam: 'camera sketch', pattern: 'needs a pattern', video: 'needs a clip' };

  function itemsOf(lib, key) {
    if (key === 'sketches') {
      return lib.sketches.map((s) => ({
        key: s.name, title: s.name, isNew: s.new, dark: false,
        sub: s.flags.filter((f) => FLAG_TEXT[f]).map((f) => FLAG_TEXT[f]).join(', '),
        picture: () => tile(s.name)
      }));
    }
    if (key === 'patterns') {
      return lib.patterns.map((p) => ({
        key: p.slug, title: p.title || p.slug, sub: p.title ? p.slug : '', isNew: p.new, dark: false,
        blocked: p.playable === false ? (p.why || 'the page cannot draw it') : null,
        picture: () => h('img', { src: p.svg, alt: '', loading: 'lazy' })
      }));
    }
    return lib.clips.map((c) => ({
      key: c.file, title: c.file, isNew: c.new, dark: true,
      sub: [formatSeconds(c.seconds), c.source === 'uploaded' ? 'uploaded' : 'from the PC'].filter(Boolean).join(' · '),
      picture: () => (c.poster ? h('img', { class: 'poster', src: c.poster, alt: '', loading: 'lazy' }) : tile('video'))
    }));
  }

  admin.registerView('random', { mount });

  function mount(root, params) {
    let alive = true, dirty = false, lib = null, tab = 'sketches';
    const model = {
      name: '', knobs: Object.assign({}, admin.knobDefaults),
      sketches: new Set(), patterns: new Set(), clips: new Set()
    };
    const bar = checkBar(toSet,
      (r) => r.playable + (r.playable === 1 ? ' sketch can play' : ' sketches can play')
        + (r.playable < 3 ? ' (3 needed)' : ''),
      save);
    const tabs = h('div', { class: 'tabs', role: 'tablist' });
    const panel = h('div', { role: 'tabpanel' });

    root.append(h('p', { class: 'hint', text: 'Loading the library' }));
    admin.library().then((l) => {
      if (!alive) return;
      lib = l;
      init();
      render();
      document.body.appendChild(bar.el);
      bar.schedule();
    }, (e) => {
      if (alive) root.replaceChildren(h('p', { class: 'hint', text: 'Could not load the library: ' + admin.describeError(e) }));
    });
    // A conversion finished while this editor was open: show the new clip
    // (off, flagged NEW) before anything can be saved, since a save marks
    // everything the service has as known.
    const stopListening = admin.onLibrary((l) => {
      if (!alive || !lib) return;
      lib = l;
      drawTabs();
      drawPanel();
      bar.schedule();
    });

    return {
      unmount() { alive = false; stopListening(); bar.stop(); bar.el.remove(); },
      leaving() { return dirty ? 'This set has unsaved changes. Leave without saving?' : null; }
    };

    function init() {
      const s = params.set;
      if (s) {
        model.name = s.name;
        model.knobs = Object.assign({}, admin.knobDefaults, s.knobs || {});
        (s.sketches || []).forEach((n) => model.sketches.add(n));
        (s.patterns || []).forEach((n) => model.patterns.add(n));
        (s.clips || []).forEach((n) => model.clips.add(n));
      } else {
        // A new set starts with everything on that can play: weeding out is
        // quicker than ticking in, and the minimum is met from the first check.
        model.name = 'New set';
        lib.sketches.forEach((x) => model.sketches.add(x.name));
        lib.patterns.filter((x) => x.playable !== false).forEach((x) => model.patterns.add(x.slug));
        lib.clips.forEach((x) => model.clips.add(x.file));
        dirty = true;
      }
    }

    function toSet() {
      return {
        name: model.name.trim(), type: 'random',
        sketches: Array.from(model.sketches), patterns: Array.from(model.patterns), clips: Array.from(model.clips),
        knobs: Object.assign({}, model.knobs)
      };
    }

    function changed() { dirty = true; bar.schedule(); }

    function render() {
      const name = h('input', { type: 'text', maxlength: 40, value: model.name, autocomplete: 'off', 'aria-label': 'Set name' });
      name.addEventListener('input', () => { model.name = name.value; changed(); });
      root.replaceChildren(
        h('h1', { text: params.set ? 'Edit random set' : 'New random set' }),
        h('label', { class: 'field' }, h('span', { text: 'Name' }), name),
        knobRow(model.knobs, changed),
        tabs, panel);
      drawTabs();
      drawPanel();
    }

    function drawTabs() {
      tabs.replaceChildren(...TABS.map((t) => {
        const total = itemsOf(lib, t.key).length;
        const on = itemsOf(lib, t.key).filter((it) => model[t.key].has(it.key)).length;
        const b = h('button', { type: 'button', role: 'tab', 'aria-selected': String(t.key === tab),
          text: t.label + ' ' + on + '/' + total });
        b.addEventListener('click', () => { tab = t.key; drawTabs(); drawPanel(); });
        return b;
      }));
    }

    function drawPanel() {
      const t = TABS.find((x) => x.key === tab);
      const items = itemsOf(lib, tab);
      const all = (on) => {
        items.forEach((it) => {
          if (!on) model[tab].delete(it.key);
          else if (!it.blocked) model[tab].add(it.key);
        });
        drawTabs(); drawPanel(); changed();
      };
      panel.replaceChildren(
        h('div', { class: 'toolbar' },
          h('button', { type: 'button', text: 'All on', onclick: () => all(true) }),
          h('button', { type: 'button', text: 'All off', onclick: () => all(false) })),
        items.length
          ? h('div', { class: 'cards' }, ...items.map((it) => card(t, it)))
          : h('p', { class: 'empty', text: 'Nothing here on the Pi.' }));
    }

    function card(t, it) {
      const on = model[t.key].has(it.key);
      const input = h('input', { type: 'checkbox', 'aria-label': 'Include ' + it.title });
      input.checked = on;
      // A pattern the page cannot draw can be unticked but not ticked.
      input.disabled = !!it.blocked && !on;
      const el = h('div', { class: 'card' + (on ? ' on' : '') + (it.blocked ? ' unplayable' : '') });
      input.addEventListener('change', () => {
        if (input.checked) model[t.key].add(it.key); else model[t.key].delete(it.key);
        el.classList.toggle('on', input.checked);
        input.disabled = !!it.blocked && !input.checked;
        drawTabs();
        changed();
      });
      const canPreview = typeof admin.preview === 'function';
      const pic = h(canPreview ? 'button' : 'div', {
        class: 'pic' + (it.dark ? ' dark' : ''),
        type: canPreview ? 'button' : null,
        'aria-label': canPreview ? 'Preview ' + it.title : null
      }, it.picture());
      if (canPreview) pic.addEventListener('click', () => admin.preview({ kind: t.kind, key: it.key }));
      el.append(pic,
        h('div', { class: 'foot' },
          h('div', { class: 'title' },
            h('div', { text: it.title }),
            it.sub ? h('div', { class: 'sub', text: it.sub }) : null,
            it.blocked ? h('div', { class: 'why', text: 'Will not play: ' + it.blocked }) : null,
            it.isNew ? h('span', { class: 'badge new', text: 'New' }) : null),
          h('label', { class: 'switch' }, input, h('span', { class: 'track' }))));
      return el;
    }

    function save() {
      return admin.saveSetAndGo(params, toSet(), bar, () => { dirty = false; }, toSet);
    }
  }
})();
