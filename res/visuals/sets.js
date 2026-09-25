// The active visuals set: which sketches, patterns and clips the show may
// use. The set is chosen on the panel ([BiteDJ],visuals_set) and arrives in
// every feed frame as settings.set; settings.setRev moves whenever the app
// sees ~/.bitedj-visuals-sets.json change. Either moving rereads the file.
// The admin service (pi/bin/bitedj-visuals-admin) is the only writer, and it
// always writes a temp file and renames it, so a read never sees half a file.
//
// Set 0 is "Everything": no file needed, everything allowed, and the
// director's own rules decide. A file that cannot be read or parsed, or
// that lacks the id asked for, also plays everything, with one log line.
// setRev is an opaque number, seeded by the app with its start time, so an
// app restart rereads the file even when the set id has not moved.
//
// Every line logged here starts 'visuals: sets '. 'visuals: set <id> ' is
// the director's switch line and nothing else, so grepping for it counts
// switches.
//
// Part A knows Random sets only. A Sequence set is logged and played as
// Everything until the sequence walker lands (Part B).
//
// The core is createSets(opts), with no browser globals, so tests can run it
// under Node with the read stubbed. The browser glue at the bottom makes the
// one instance the page uses, window.sets.
(function () {
  const EVERYTHING = Object.freeze({ id: 0, name: 'Everything', type: 'random' });
  const KINDS = { sketch: 'sketches', pattern: 'patterns', clip: 'clips' };

  // { doc } or { error }.
  function parse(text) {
    let doc;
    try {
      doc = JSON.parse(text);
    } catch (e) {
      return { error: 'not JSON: ' + e.message };
    }
    if (!doc || typeof doc !== 'object' || doc.version !== 1 || !Array.isArray(doc.sets)) {
      return { error: 'not a version 1 sets file' };
    }
    return { doc };
  }

  const strings = (v) => new Set(Array.isArray(v) ? v.filter(x => typeof x === 'string') : []);

  function createSets(opts) {
    const log = opts.log || (() => {});
    const listeners = [];
    // What the last update() asked for; null until the first one.
    let askedSet = null, askedRev = null;
    // Bumped by every update() that starts a load, so a read that completes
    // after a newer one started is recognised and dropped.
    let generation = 0;
    // What is in force.
    let active = EVERYTHING;
    let allow = null;            // null: everything; else { sketches, patterns, clips } of Sets

    function notify(info) {
      listeners.forEach((fn) => {
        try { fn(info); } catch (e) { log('visuals: sets listener failed', e && e.message ? e.message : String(e)); }
      });
    }

    // Puts `next` in force and tells the listeners.
    function commit(next, nextAllow, reason) {
      const previousId = active.id;
      active = next;
      allow = nextAllow;
      notify({ id: active.id, previousId, type: active.type, reason });
    }

    // A completed read for set `id`.
    function loaded(id, reason, err, text) {
      const parsed = err ? { error: err.message || String(err) } : parse(text);
      if (parsed.error) {
        log('visuals: sets unreadable (' + parsed.error + '); playing everything');
        commit(EVERYTHING, null, 'unreadable');
        return;
      }
      const s = parsed.doc.sets.find(x => x && x.id === id);
      if (!s) {
        log('visuals: sets ' + id + ' is not in the sets file; playing everything');
        commit(EVERYTHING, null, reason);
        return;
      }
      if (s.type === 'random') {
        commit({ id, name: String(s.name), type: 'random' },
          { sketches: strings(s.sketches), patterns: strings(s.patterns), clips: strings(s.clips) },
          reason);
        return;
      }
      if (s.type === 'sequence') {
        log('visuals: sets ' + id + ' is a sequence; sequences are not supported by this page, playing everything');
      } else {
        log('visuals: sets ' + id + ' has an unknown type (' + s.type + '); playing everything');
      }
      commit(EVERYTHING, null, reason);
    }

    return {
      update(settings) {
        const id = Number(settings && settings.set) || 0;
        const rev = Number(settings && settings.setRev) || 0;
        if (id === askedSet && rev === askedRev) return;
        const reason = askedSet === null || id !== askedSet ? 'switch' : 'edit';
        askedSet = id;
        askedRev = rev;
        const mine = ++generation;
        if (id === 0) {
          commit(EVERYTHING, null, reason);
          return;
        }
        if (!opts.path) return;
        opts.read(opts.path, (err, text) => {
          if (mine !== generation) return;
          loaded(id, reason, err, text);
        });
      },
      onChange(fn) {
        if (typeof fn === 'function') listeners.push(fn);
      },
      active() {
        return active;
      },
      allows(kind, key) {
        if (!allow) return true;
        const list = allow[KINDS[kind]];
        return !!list && list.has(key);
      },
      isSequence() {
        return false;
      }
    };
  }

  if (typeof module !== 'undefined' && module.exports) {
    module.exports = { createSets };
  } else {
    const paths = window.visualsPaths || {};
    window.sets = createSets({
      path: paths.setsFile || null,
      read: (p, cb) => paths.readText(p, cb),
      log: (...a) => console.log(...a)
    });
  }
})();
