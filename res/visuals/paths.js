// Where the page's files live, and the one way it reads them.
//
// On the Pi the page is file:///home/blaise/bitedj-res/visuals/index.html,
// so the sets file (~/.bitedj-visuals-sets.json) and the uploads
// (~/bitedj-media/) are two directories up. Through the admin service the
// page is http://<pi>:7380/visuals/index.html and the uploads are under
// /media/; a preview there never needs the sets file, so setsFile is null
// and sets.js plays everything. `?setsfile=<url>` overrides that for a
// desktop check (python -m http.server from res/, with a fixture file).
//
// readText is XHR and not fetch(): Chromium refuses fetch() of a file:// URL
// even with --allow-file-access-from-files, and XHR is what patterns.js has
// always used to read its SVGs. A file:// XHR answers status 0, not 200, so
// status 0 with text is success and status 0 with nothing is a missing file.
(function () {
  const onFile = location.protocol === 'file:';
  const override = new URLSearchParams(location.search).get('setsfile');
  window.visualsPaths = {
    media: onFile ? '../../bitedj-media/' : '../media/',
    setsFile: override || (onFile ? '../../.bitedj-visuals-sets.json' : null),
    // cb(err, text). Over http a query string defeats the cache, since the
    // service may rewrite the file between two reads a second apart.
    readText(path, cb) {
      let done = false;
      const finish = (err, text) => { if (!done) { done = true; cb(err, text); } };
      const url = onFile ? path : path + (path.indexOf('?') < 0 ? '?' : '&') + 't=' + Date.now();
      const xhr = new XMLHttpRequest();
      xhr.onload = () => {
        const text = xhr.responseText;
        if (xhr.status === 200 || (xhr.status === 0 && text)) finish(null, text);
        else finish(new Error(xhr.status ? 'status ' + xhr.status : 'missing or empty'), null);
      };
      xhr.onerror = () => finish(new Error('not readable'), null);
      try {
        xhr.open('GET', url, true);
        xhr.overrideMimeType('text/plain');
        xhr.send();
      } catch (e) {
        finish(e, null);
      }
    }
  };
})();
