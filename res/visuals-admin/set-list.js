// The Sets view: every set with its type, how many items or entries it has,
// and which one is on the panel now. The panel picks the active set; this
// page only builds them, so there is no "activate" button here. Everything
// (id 0) is built in: it can be duplicated into an editable Random set but
// not edited or deleted. "New sequence" appears only once sequence-editor.js
// has registered its view.
(function () {
  const { h } = admin;

  admin.registerView('sets', { mount });

  function mount(root) {
    let alive = true;
    const list = h('ul', { class: 'set-list', 'aria-live': 'polite' });
    root.append(
      h('h1', { text: 'Visuals sets' }),
      h('p', { class: 'hint', text: 'Build sets here. ' + admin.panelWhere }),
      h('div', { class: 'toolbar' },
        h('a', { class: 'button primary', href: '#new/random', text: 'New random set' }),
        admin.views.sequence ? h('a', { class: 'button', href: '#new/sequence', text: 'New sequence' }) : null),
      list);
    load();
    return { unmount() { alive = false; } };

    async function load() {
      list.replaceChildren(h('li', { class: 'empty', text: 'Loading the sets' }));
      let data;
      try {
        data = await api.sets();
      } catch (e) {
        if (alive) list.replaceChildren(h('li', { class: 'empty', text: 'Could not load the sets: ' + admin.describeError(e) }));
        return;
      }
      if (!alive) return;
      list.replaceChildren(...data.sets.map((s) => row(s, s.id === data.active)));
    }

    function row(s, active) {
      const kind = s.type === 'sequence' ? 'Sequence' : 'Random';
      const count = s.type === 'sequence'
        ? s.count + (s.count === 1 ? ' entry' : ' entries')
        : s.count + (s.count === 1 ? ' item' : ' items');
      const main = h(s.locked ? 'div' : 'a', { class: 'main', href: s.locked ? null : '#set/' + s.id },
        h('span', { class: 'name', text: s.name }),
        h('span', { class: 'meta' },
          kind + ' · ' + count + (s.locked ? ' · built in' : '') + ' ',
          active ? h('span', { class: 'badge active', text: 'On the panel' }) : null));
      return h('li', { class: 'set-row' + (active ? ' active' : '') },
        main,
        h('div', { class: 'row-actions' },
          h('button', { type: 'button', text: 'Duplicate', onclick: () => duplicate(s) }),
          s.locked ? null : h('button', { type: 'button', class: 'danger', text: 'Delete', onclick: () => remove(s, active) })));
    }

    async function duplicate(s) {
      try {
        let copy;
        if (s.id === 0) {
          const lib = await admin.library();
          copy = {
            name: 'Copy of Everything', type: 'random',
            sketches: lib.sketches.map((x) => x.name),
            patterns: lib.patterns.map((x) => x.slug),
            clips: lib.clips.map((x) => x.file),
            knobs: Object.assign({}, admin.knobDefaults)
          };
        } else {
          copy = JSON.parse(JSON.stringify(s));
          delete copy.id; delete copy.count; delete copy.locked;
          copy.name = ('Copy of ' + s.name).slice(0, 40);
        }
        const r = await api.createSet(copy);
        admin.invalidateLibrary();
        admin.toast('Made "' + r.set.name + '"');
        admin.go('#set/' + r.set.id);
      } catch (e) {
        admin.toast('Could not duplicate: ' + admin.describeError(e), 'error');
      }
    }

    async function remove(s, active) {
      const warn = 'Delete "' + s.name + '"?'
        + (active ? ' It is the set on the panel now, so the TV falls back to Everything.' : '')
        + ' This cannot be undone.';
      if (!confirm(warn)) return;
      try {
        await api.deleteSet(s.id);
        admin.toast('Deleted "' + s.name + '"');
        load();
      } catch (e) {
        admin.toast('Could not delete: ' + admin.describeError(e), 'error');
      }
    }
  }
})();
