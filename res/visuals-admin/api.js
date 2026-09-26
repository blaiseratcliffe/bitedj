// Talks to pi/bin/bitedj-visuals-admin. Every call is same-origin, so the
// session cookie rides along by itself. A 401 means the session is gone (it
// expired, or the PIN was changed on the panel, which drops every session)
// and sends the browser to the login form. A failed call throws an ApiError
// whose `problems` is the service's list when it sent one, which is how a
// rejected save tells the editor why.
//
// Uploads use XMLHttpRequest rather than fetch because fetch still has no
// upload progress. The body is the raw file and the original name travels
// percent-encoded in X-Filename (plan decision 12: no multipart). Ask
// clipSpace() first: a refusal the service sends while a large body is still
// going out reaches a Windows browser as a connection reset, with no status,
// so "not enough space" would read as "cut off".
(function () {
  class ApiError extends Error {
    constructor(status, body) {
      super(body && body.error ? String(body.error)
        : status ? 'HTTP ' + status : 'No answer from the Pi');
      this.name = 'ApiError';
      this.status = status;
      this.body = body || null;
      this.problems = body && Array.isArray(body.problems) ? body.problems.map(String) : [];
    }
  }

  function toLogin() { location.href = '/login'; }

  function parseBody(text) {
    if (!text) return null;
    try { return JSON.parse(text); } catch (e) { return { error: text.slice(0, 300) }; }
  }

  async function call(method, path, body) {
    const init = { method, credentials: 'same-origin', headers: { Accept: 'application/json' } };
    if (body !== undefined) {
      init.headers['Content-Type'] = 'application/json';
      init.body = JSON.stringify(body);
    }
    let res;
    try {
      res = await fetch(path, init);
    } catch (e) {
      throw new ApiError(0, { error: 'No answer from the Pi. Is it on, and on this network?' });
    }
    const data = parseBody(await res.text());
    if (res.status === 401) { toLogin(); throw new ApiError(401, data); }
    if (!res.ok) throw new ApiError(res.status, data);
    return data;
  }

  const clipPath = (file) => '/api/clips/' + encodeURIComponent(file);
  let inFlight = 0;

  function uploadClip(file, onProgress) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      inFlight += 1;
      const finish = (fn, v) => { inFlight -= 1; fn(v); };
      xhr.open('POST', '/api/clips');
      xhr.setRequestHeader('X-Filename', encodeURIComponent(file.name));
      xhr.setRequestHeader('Accept', 'application/json');
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable && onProgress) onProgress(e.loaded / e.total);
      };
      xhr.onload = () => {
        const data = parseBody(xhr.responseText);
        if (xhr.status === 401) { toLogin(); finish(reject, new ApiError(401, data)); return; }
        if (xhr.status >= 200 && xhr.status < 300) finish(resolve, data);
        else finish(reject, new ApiError(xhr.status, data));
      };
      xhr.onerror = () => finish(reject,
        new ApiError(0, { error: 'The upload was cut off. Is the Pi still on this network?' }));
      xhr.onabort = () => finish(reject, new ApiError(0, { error: 'Upload cancelled' }));
      xhr.send(file);
    });
  }

  window.api = {
    ApiError,
    library: () => call('GET', '/api/library'),
    sets: () => call('GET', '/api/sets'),
    check: (set) => call('POST', '/api/sets/check', set),
    createSet: (set) => call('POST', '/api/sets', set),
    saveSet: (id, set) => call('PUT', '/api/sets/' + id, set),
    deleteSet: (id) => call('DELETE', '/api/sets/' + id),
    clipUsage: (file) => call('GET', clipPath(file) + '/usage'),
    deleteClip: (file) => call('DELETE', clipPath(file)),
    jobs: () => call('GET', '/api/jobs'),
    clipSpace: (bytes) => call('GET', '/api/clips/space?bytes=' + Math.max(0, Math.floor(bytes))),
    uploadClip,
    uploadsInFlight: () => inFlight
  };
})();
