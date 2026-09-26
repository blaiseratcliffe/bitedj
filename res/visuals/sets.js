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
// A Random set narrows the pools; a Sequence is a running order that next()
// walks, intro entries once and the rest on a loop. Mixxx never reads the
// entries; this file does all the ordering.
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

  // Two sequence entries, as loaded() shapes them, name the same thing.
  const sameEntry = (a, b) => !!a && !!b && a.sketch === b.sketch && a.pattern === b.pattern &&
    a.clip === b.clip && a.bars === b.bars && a.intro === b.intro;

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
    // The active sequence's entries, or null. `pos` is the index next()
    // starts looking from; `last` the index it returned last, or -1.
    let entries = null, loopStart = 0, pos = 0, last = -1;

    function notify(info) {
      listeners.forEach((fn) => {
        try { fn(info); } catch (e) { log('visuals: sets listener failed', e && e.message ? e.message : String(e)); }
      });
    }

    // Puts `next` in force and tells the listeners.
    function commit(next, nextAllow, reason, nextEntries) {
      const previousId = active.id;
      const sameSequence = !!entries && !!nextEntries && next.id === previousId && reason === 'edit';
      // Whether the walk had finished the intros, judged before the edit
      // moves loopStart.
      const pastIntros = pos >= loopStart;
      const lastEntry = entries && last >= 0 ? entries[last] : null;
      active = next;
      allow = nextAllow;
      if (nextEntries) {
        entries = nextEntries;
        let i = 0;
        while (i < entries.length && entries[i].intro) i += 1;
        loopStart = i;
        if (sameSequence) {
          // Keep the place; past the end wraps to the loop, and a place still
          // inside the intros stays there. A walk that was past the intros
          // stays past them, even when the edit added some in front of it.
          if (pos >= entries.length) pos = loopStart;
          if (pastIntros && pos < loopStart) pos = loopStart;
          // replay() resumes the interrupted entry only where it was: the
          // same entry (all five fields) at the same index, and not an intro
          // once the walk is past the intros. Anything else forgets it, and
          // replay() is then next(), which resumes from `pos`.
          if (last >= entries.length || !sameEntry(entries[last], lastEntry) ||
              (entries[last].intro && pos >= loopStart)) last = -1;
        } else {
          pos = 0;
          last = -1;
        }
      } else {
        entries = null;
        loopStart = 0; pos = 0; last = -1;
      }
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
        const list = (Array.isArray(s.entries) ? s.entries : [])
          .filter(e => e && typeof e.sketch === 'string')
          .map(e => ({
            sketch: e.sketch,
            pattern: typeof e.pattern === 'string' ? e.pattern : null,
            clip: typeof e.clip === 'string' ? e.clip : null,
            bars: Number.isInteger(e.bars) && e.bars > 0 ? e.bars : null,
            intro: e.intro === true
          }));
        if (!list.length) {
          log('visuals: sets ' + id + ' is a sequence with no entries; playing everything');
          commit(EVERYTHING, null, reason);
          return;
        }
        commit({ id, name: String(s.name), type: 'sequence' }, {
          sketches: new Set(list.map(e => e.sketch)),
          patterns: new Set(list.filter(e => e.pattern).map(e => e.pattern)),
          clips: new Set(list.filter(e => e.clip).map(e => e.clip))
        }, reason, list);
        return;
      }
      log('visuals: sets ' + id + ' has an unknown type (' + s.type + '); playing everything');
      commit(EVERYTHING, null, reason);
    }

    const entryAt = (i) => Object.assign({}, entries[i], { index: i + 1, total: entries.length });

    // `call` is one per next(), peek() or replay() call, so a canPlay that
    // throws on every entry logs one line per call, not one per entry.
    function playable(canPlay, i, call) {
      try {
        return !!canPlay(entryAt(i));
      } catch (e) {
        if (!call.logged) {
          call.logged = true;
          // 'visuals: sets ', like every line this file logs (contract 1.3).
          log('visuals: sets ' + active.id + ' entry ' + (i + 1) + ' check failed:', e && e.message ? e.message : String(e));
        }
        return false;
      }
    }

    // The index next() would return from `from`, or -1. Intros are tried in
    // order once; the loop is tried for at most one pass.
    function scan(canPlay, from, call) {
      if (!entries) return -1;
      const n = entries.length;
      let i = from;
      while (i < loopStart) {
        if (playable(canPlay, i, call)) return i;
        i += 1;
      }
      const loopLen = n - loopStart;
      if (loopLen <= 0) return -1;
      const startAt = i >= n ? loopStart : i;
      for (let k = 0; k < loopLen; k++) {
        const j = loopStart + ((startAt - loopStart + k) % loopLen);
        if (playable(canPlay, j, call)) return j;
      }
      return -1;
    }

    // next() itself, shared with replay() so neither depends on `this`.
    function nextEntry(canPlay, call) {
      const j = scan(canPlay, pos, call);
      if (j < 0) {
        // Intros that could not play are passed for good, even when nothing
        // after them can play either.
        if (pos < loopStart) pos = loopStart;
        return null;
      }
      last = j;
      pos = j + 1;
      return entryAt(j);
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
        return !!entries;
      },
      restart() {
        pos = 0;
        last = -1;
      },
      next(canPlay) {
        return nextEntry(canPlay, { logged: false });
      },
      peek(canPlay) {
        const j = scan(canPlay, pos, { logged: false });
        return j < 0 ? null : entryAt(j);
      },
      replay(canPlay) {
        const call = { logged: false };
        if (last >= 0 && last < (entries ? entries.length : 0) && playable(canPlay, last, call)) return entryAt(last);
        return nextEntry(canPlay, call);
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
