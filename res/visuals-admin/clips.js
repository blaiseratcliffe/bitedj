// The Clips view. Upload a clip and watch it upload and convert on the Pi;
// delete clips that were uploaded here.
// Clips deployed from the PC (res/visuals/assets/video/) are listed but
// cannot be deleted from this page; deploy-skin.sh would only put them back.
//
// The upload queue is kept at file level rather than inside the view: the
// admin page is a single document, so an upload carries on while the hash
// changes, and coming back to #clips shows it where it is. One upload at a
// time, since the Pi converts one at a time anyway.
//
// Each upload asks GET /api/clips/space first. A refusal the service sends
// while a large body is still going out reaches a Windows browser as a
// connection reset with no status, so asking first is the only way "not
// enough space" reads as that. Conversion progress comes from app.js's job
// poll, which runs whatever view is open.
(function () {
  const { h } = admin;
  const MAX_BYTES = 1073741824;
  const EXTENSIONS = ['.mp4', '.mov', '.m4v', '.webm', '.mkv', '.avi'];

  const queue = [];     // { file, state: 'waiting' | 'checking' | 'uploading' | 'failed', progress, error }
  let uploading = null;
  let redraw = null;    // the mounted view's repaint, or null

  function extensionOf(name) {
    const i = name.lastIndexOf('.');
    return i < 0 ? '' : name.slice(i).toLowerCase();
  }

  function refusal(file) {
    if (file.size > MAX_BYTES) return 'larger than 1 GB';
    if (!EXTENSIONS.includes(extensionOf(file.name))) return 'not one of ' + EXTENSIONS.join(' ');
    return null;
  }

  function uploadError(e) {
    if (e.status === 413) return 'larger than the 1 GB the Pi accepts';
    if (e.status === 415) return 'not a video file the Pi converts (' + EXTENSIONS.join(' ') + ')';
    if (e.status === 507) return 'not enough free space on the Pi (' + e.message + ')';
    return admin.describeError(e);
  }

  function enqueue(files) {
    for (const file of files) {
      const why = refusal(file);
      queue.push(why ? { file, state: 'failed', progress: 0, error: why } : { file, state: 'waiting', progress: 0, error: null });
    }
    if (redraw) redraw();
    pump();
  }

  async function pump() {
    if (uploading) return;
    const item = queue.find((q) => q.state === 'waiting');
    if (!item) return;
    uploading = item;
    try {
      item.state = 'checking';
      if (redraw) redraw();
      await api.clipSpace(item.file.size);
      item.state = 'uploading';
      if (redraw) redraw();
      await api.uploadClip(item.file, (f) => { item.progress = f; if (redraw) redraw(); });
      queue.splice(queue.indexOf(item), 1);   // the job list shows it from here on
      admin.watchJobs();
    } catch (e) {
      item.state = 'failed';
      item.error = uploadError(e);
    } finally {
      uploading = null;
      if (redraw) redraw();
      pump();
    }
  }

  // A clip's length as m:ss: random-editor.js's (Task A15), looked up at
  // call time so the order the two scripts load in does not matter.
  const formatSeconds = (s) => admin.formatSeconds(s);

  function stateText(j) {
    if (j.state === 'queued') return 'Waiting to convert';
    if (j.state === 'converting') return 'Converting ' + Math.round((j.progress || 0) * 100) + '%';
    if (j.state === 'done') return 'Ready as ' + j.output;
    return 'Conversion failed';
  }

  admin.registerView('clips', { mount });

  function mount(root) {
    const input = h('input', { type: 'file', accept: 'video/*,' + EXTENSIONS.join(','), multiple: true, hidden: true });
    input.addEventListener('change', () => { enqueue(Array.from(input.files)); input.value = ''; });
    const pick = h('button', { type: 'button', class: 'primary', text: 'Choose clips', onclick: () => input.click() });
    const zone = h('div', { class: 'dropzone' },
      h('div', { text: 'Drop video files here, or' }), pick, input,
      h('div', { class: 'hint', text: 'Up to 1 GB each. The Pi converts them to 640x360 with no sound, one at a time.' }));
    zone.addEventListener('dragover', (e) => { e.preventDefault(); zone.classList.add('over'); });
    zone.addEventListener('dragleave', () => zone.classList.remove('over'));
    zone.addEventListener('drop', (e) => {
      e.preventDefault();
      zone.classList.remove('over');
      enqueue(Array.from(e.dataTransfer.files || []));
    });

    const uploadsList = h('ul', { class: 'stack' });
    const jobsList = h('ul', { class: 'stack' });
    const clipsList = h('ul', { class: 'stack' });
    const jobsHead = h('h2', { text: 'Conversions' });
    root.append(h('h1', { text: 'Clips' }), zone, uploadsList, jobsHead, jobsList, h('h2', { text: 'On the Pi' }), clipsList);

    let alive = true;
    redraw = () => { if (alive) { drawUploads(); drawJobs(admin.jobs()); } };
    const stopJobs = admin.onJobs(drawJobs);
    // A finished conversion: app.js has already reloaded the library.
    const stopLibrary = admin.onLibrary(() => loadClips());
    loadClips();
    redraw();
    admin.watchJobs();
    return {
      unmount() {
        alive = false;
        redraw = null;
        stopJobs();
        stopLibrary();
      }
    };

    function drawUploads() {
      uploadsList.replaceChildren(...queue.map((q) => {
        const bar = h('div', { class: 'progress' }, h('span', { style: { width: Math.round(q.progress * 100) + '%' } }));
        const text = q.state === 'waiting' ? 'Waiting to upload'
          : q.state === 'checking' ? 'Checking space on the Pi'
            : q.state === 'uploading' ? 'Uploading ' + Math.round(q.progress * 100) + '%'
              : 'Not uploaded: ' + q.error;
        return h('li', { class: 'upload-row' },
          h('div', { text: q.file.name }),
          h('div', { class: 'hint', text }),
          q.state === 'failed'
            ? h('button', { type: 'button', text: 'Dismiss', onclick: () => { queue.splice(queue.indexOf(q), 1); drawUploads(); } })
            : bar);
      }));
    }

    function drawJobs(jobs) {
      if (!alive) return;
      jobsHead.hidden = jobs.length === 0;
      jobsList.replaceChildren(...jobs.map((j) => h('li', { class: 'job-row' + (j.state === 'failed' ? ' failed' : '') },
        h('div', { text: j.name }),
        h('div', { class: 'state', text: stateText(j) }),
        j.state === 'converting'
          ? h('div', { class: 'progress' }, h('span', { style: { width: Math.round((j.progress || 0) * 100) + '%' } }))
          : null,
        j.state === 'failed' && j.error
          ? h('details', null, h('summary', { text: 'What ffmpeg said' }), h('pre', { text: j.error }))
          : null)));
    }

    async function loadClips() {
      let lib;
      try {
        lib = await admin.library();
      } catch (e) {
        if (alive) clipsList.replaceChildren(h('li', { class: 'empty', text: 'Could not load the clips: ' + admin.describeError(e) }));
        return;
      }
      if (!alive) return;
      if (!lib.clips.length) {
        clipsList.replaceChildren(h('li', { class: 'empty', text: 'No clips on the Pi yet.' }));
        return;
      }
      clipsList.replaceChildren(...lib.clips.map(clipRow));
    }

    function clipRow(c) {
      const canPreview = typeof admin.preview === 'function';
      const thumb = h(canPreview ? 'button' : 'div', {
        class: 'thumb', type: canPreview ? 'button' : null, 'aria-label': canPreview ? 'Preview ' + c.file : null
      }, c.poster ? h('img', { src: c.poster, alt: '', loading: 'lazy' }) : null);
      if (canPreview) thumb.addEventListener('click', () => admin.preview({ kind: 'clip', key: c.file }));
      const uploaded = c.source === 'uploaded';
      return h('li', { class: 'clip-row' },
        thumb,
        h('div', { class: 'info' },
          h('div', null, c.file, ' ', c.new ? h('span', { class: 'badge new', text: 'New' }) : null),
          h('div', { class: 'meta', text: [formatSeconds(c.seconds),
            uploaded ? 'uploaded here' : 'deployed from the PC, delete it there'].filter(Boolean).join(' · ') })),
        uploaded
          ? h('button', { type: 'button', class: 'danger', text: 'Delete', onclick: () => remove(c) })
          : null);
    }

    async function remove(c) {
      let usage;
      try {
        usage = await api.clipUsage(c.file);
      } catch (e) {
        admin.toast('Could not check where ' + c.file + ' is used: ' + admin.describeError(e), 'error');
        return;
      }
      const parts = ['Delete ' + c.file + '?'];
      if (usage.random.length) {
        parts.push('It is ticked in ' + usage.random.length + (usage.random.length === 1 ? ' Random set' : ' Random sets')
          + ' (' + usage.random.map((s) => s.name).join(', ') + ') and will be unticked.');
      }
      if (usage.sequences.length) {
        const n = usage.sequences.reduce((a, s) => a + s.entries, 0);
        parts.push(n + (n === 1 ? ' sequence entry uses' : ' sequence entries use') + ' it ('
          + usage.sequences.map((s) => s.name).join(', ') + '); '
          + (n === 1 ? 'that entry is' : 'those entries are') + ' removed.');
      }
      parts.push('This cannot be undone.');
      if (!confirm(parts.join(' '))) return;
      try {
        await api.deleteClip(c.file);
        admin.invalidateLibrary();
        admin.toast('Deleted ' + c.file);
        loadClips();
      } catch (e) {
        admin.toast('Could not delete ' + c.file + ': ' + admin.describeError(e), 'error');
      }
    }
  }
})();
