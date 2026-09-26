// The admin page's frame: the view registry, a hash router, the header's
// current-section marker, a banner for warnings that outlive a view, toasts,
// a cached GET /api/library, and the conversion-job poll.
//
// The job poll runs here, whatever view is open, because the service marks
// everything in its library "known" on every save. A clip that finished
// converting while an editor held an older library would be saved as known
// without ever having been shown, and never get its NEW badge. So a finished
// job reloads the library at once and hands it to every view that listens
// (admin.onLibrary), and the view redraws before anything can be saved.
//
// Each view lives in its own file and calls admin.registerView(name, view)
// as it loads; admin.start() runs after every script tag. A view file that
// is missing (sequence-editor.js before Part B) is a 404 and nothing more:
// a hash that needs it shows "Not available". A view is { mount(root,
// params) }, and mount may return { unmount(), leaving() }. unmount runs
// when the router moves on; leaving() returns a message when moving on would
// lose work, and the router asks before it goes.
(function () {
  const views = {};
  const banners = new Map();
  let current = null;       // what the mounted view's mount() returned
  let currentHash = null;
  let ignoreHash = null;    // the hash the router itself put back after a "stay"
  let routeToken = 0;       // an async route that lost the race drops its result
  let libraryPromise = null;
  const libraryListeners = new Set();
  const jobListeners = new Set();
  let jobs = [];
  let jobTimer = null, jobPolling = false, jobAgain = false;
  const seenJobs = new Map();   // job id -> last state seen; the first poll only records

  function h(tag, props, ...children) {
    const el = document.createElement(tag);
    if (props) {
      for (const [k, v] of Object.entries(props)) {
        if (v === null || v === undefined || v === false) continue;
        if (k === 'class') el.className = v;
        else if (k === 'text') el.textContent = v;
        else if (k === 'style' && typeof v === 'object') Object.assign(el.style, v);
        else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2), v);
        else if (v === true) el.setAttribute(k, '');
        else el.setAttribute(k, String(v));
      }
    }
    for (const c of children.flat()) {
      if (c === null || c === undefined || c === false) continue;
      el.appendChild(typeof c === 'string' || typeof c === 'number'
        ? document.createTextNode(String(c)) : c);
    }
    return el;
  }

  function describeError(e) {
    if (e && Array.isArray(e.problems) && e.problems.length) return e.problems.join('; ');
    return e && e.message ? e.message : String(e);
  }

  function setBanner(key, text) {
    if (text) banners.set(key, text); else banners.delete(key);
    const el = document.getElementById('banner');
    el.replaceChildren(...Array.from(banners.values()).map((t) => h('p', { text: t })));
    el.hidden = banners.size === 0;
  }

  function toast(text, kind) {
    const t = h('div', { class: 'toast' + (kind === 'error' ? ' error' : ''), role: 'status', text });
    document.getElementById('toasts').appendChild(t);
    setTimeout(() => t.remove(), kind === 'error' ? 7000 : 3000);
  }

  // One request shared by every caller until something invalidates it (a
  // save adds items to `known`, so NEW flags change; an upload adds a clip).
  function library(force) {
    if (force || !libraryPromise) {
      libraryPromise = api.library().then((lib) => {
        setBanner('sketch-list', lib.sketchListError
          ? 'The service could not read the sketch list in sketches.js ('
            + lib.sketchListError + '), so sketch names on this page may be wrong.'
          : null);
        return lib;
      });
      libraryPromise.catch(() => { libraryPromise = null; });
    }
    return libraryPromise;
  }

  function listen(set, fn) {
    set.add(fn);
    return () => set.delete(fn);
  }

  function tell(set, value) {
    for (const fn of Array.from(set)) {
      try { fn(value); } catch (e) { console.error('admin: listener failed', e); }
    }
  }

  const ACTIVE_JOB = { queued: true, converting: true };

  // Polls GET /api/jobs every second while a job is queued or converting or
  // an upload is running, and stops otherwise; clips.js calls watchJobs()
  // when an upload is accepted. A job seen to reach 'done' reloads the
  // library and tells the listening views. Jobs already done when the page
  // loaded were finished before any library this page holds was fetched, so
  // the first poll only records them.
  async function pollJobs() {
    if (jobPolling) { jobAgain = true; return; }
    jobPolling = true;
    clearTimeout(jobTimer);
    jobTimer = null;
    let delay = 3000;   // after a failed poll: the network dropped, a job may still be running
    try {
      const r = await api.jobs();
      jobs = r.jobs || [];
      let finished = false;
      for (const j of jobs) {
        const before = seenJobs.get(j.id);
        if (j.state === 'done' && before !== undefined && before !== 'done') finished = true;
        seenJobs.set(j.id, j.state);
      }
      tell(jobListeners, jobs);
      if (finished) {
        const lib = await library(true);
        tell(libraryListeners, lib);
      }
      delay = jobs.some((j) => ACTIVE_JOB[j.state]) || api.uploadsInFlight() > 0 ? 1000 : 0;
    } catch (e) {
      // Keep trying after a network drop; a 401 has already sent the browser
      // to the login form. A 404 means a service without the jobs route
      // (A14 can land before A12): stop until an upload asks again.
      if (e && e.status === 404) delay = 0;
    } finally {
      jobPolling = false;
      if (jobAgain) { jobAgain = false; delay = 1; }
      if (delay) jobTimer = setTimeout(pollJobs, delay);
    }
  }

  function parse(hash) {
    const parts = hash.replace(/^#\/?/, '').split('/').filter(Boolean).map(decodeURIComponent);
    if (parts[0] === 'clips') return { view: 'clips' };
    if (parts[0] === 'new' && (parts[1] === 'random' || parts[1] === 'sequence')) {
      return { view: parts[1] };
    }
    if (parts[0] === 'set' && /^\d+$/.test(parts[1] || '')) return { view: 'set', id: Number(parts[1]) };
    return { view: 'sets' };
  }

  function note(root, title, text) {
    root.replaceChildren(
      h('h1', { text: title }),
      h('p', { class: 'hint', text }),
      h('p', null, h('a', { class: 'button', href: '#sets', text: 'Back to the sets' })));
  }

  function mountView(name, params) {
    const root = document.getElementById('view');
    const view = views[name];
    if (!view) {
      note(root, 'Not available', 'This part of the page is not installed on the Pi yet.');
      return null;
    }
    root.replaceChildren();
    try {
      return view.mount(root, params) || null;
    } catch (e) {
      console.error('admin: view', name, 'failed', e);
      note(root, 'Something went wrong', describeError(e));
      return null;
    }
  }

  function markNav(section) {
    document.querySelectorAll('[data-nav]').forEach((a) => {
      if (a.dataset.nav === section) a.setAttribute('aria-current', 'page');
      else a.removeAttribute('aria-current');
    });
  }

  async function route() {
    const hash = location.hash || '#sets';
    if (ignoreHash === hash) { ignoreHash = null; return; }
    const warn = current && typeof current.leaving === 'function' ? current.leaving() : null;
    if (warn && currentHash !== null && hash !== currentHash && !confirm(warn)) {
      ignoreHash = currentHash;
      location.hash = currentHash;
      return;
    }
    if (current && typeof current.unmount === 'function') {
      try { current.unmount(); } catch (e) { console.error('admin: unmount failed', e); }
    }
    current = null;
    currentHash = hash;
    const r = parse(hash);
    markNav(r.view === 'clips' ? 'clips' : 'sets');
    const token = ++routeToken;
    if (r.view !== 'set') {
      current = mountView(r.view, { id: null, set: null, active: null });
      return;
    }
    const root = document.getElementById('view');
    root.replaceChildren(h('p', { class: 'hint', text: 'Loading the set' }));
    let data;
    try {
      data = await api.sets();
    } catch (e) {
      if (token === routeToken) note(root, 'Could not load the sets', describeError(e));
      return;
    }
    if (token !== routeToken) return;
    if (r.id === 0) {
      note(root, 'Everything', 'Everything plays every item on the Pi and leaves the knobs as they '
        + 'are. It cannot be edited; duplicate it from the set list to make a set you can change.');
      return;
    }
    const set = data.sets.find((s) => s.id === r.id);
    if (!set) {
      note(root, 'Not found', 'Set ' + r.id + ' does not exist any more.');
      return;
    }
    current = mountView(set.type === 'sequence' ? 'sequence' : 'random',
      { id: set.id, set, active: data.active });
  }

  function go(hash, opts) {
    if (location.hash === hash) {
      if (opts && opts.reload) route();
    } else {
      location.hash = hash;
    }
  }

  window.addEventListener('beforeunload', (e) => {
    const warn = (current && typeof current.leaving === 'function' ? current.leaving() : null)
      || (api.uploadsInFlight() > 0 ? 'An upload is still running.' : null);
    if (warn) { e.preventDefault(); e.returnValue = warn; }
  });

  window.admin = {
    h,
    views,
    registerView(name, view) { views[name] = view; },
    library,
    invalidateLibrary() { libraryPromise = null; },
    onLibrary: (fn) => listen(libraryListeners, fn),
    jobs: () => jobs,
    onJobs: (fn) => listen(jobListeners, fn),
    watchJobs: pollJobs,
    go,
    toast,
    setBanner,
    describeError,
    knobDefaults: Object.freeze({ bars: 16, reactivity: 1, bounce: 2, swirl: 2, camMix: 1 }),
    // Where the panel picks the active set.
    panelWhere: 'The panel picks which set plays: Settings, Visuals, Library, then Choose on the Set row.',
    start() {
      window.addEventListener('hashchange', route);
      route();
      library().catch(() => {});   // for the sketch-list banner; views ask again
      pollJobs();                  // a conversion may be running from an earlier visit
    }
  };
})();
