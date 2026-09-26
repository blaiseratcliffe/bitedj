"""tools/build-pattern-index.py with Chrome stubbed out.

Part B final review, finding 5: a failed cover measurement used to leave
behind an index.js without `cover`, which the admin service reads as "hide
nothing" (Decision 34), so the next deploy put every too-solid pattern back
in the library. Every way the measurement can fail must now leave index.js
exactly as it was and exit non-zero with a message that says so.

The script runs against a two-pattern fixture in a temp directory; its
PATTERNS, SRC and DST point there, and subprocess.run is replaced, so no
browser starts and the real assets/patterns/index.js is never touched.

    python tests/test_build_pattern_index.py        # from res/visuals
"""

import contextlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import urllib.parse
import urllib.request
from unittest import mock

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
VISUALS = os.path.dirname(HERE)
SCRIPT = os.path.join(VISUALS, 'tools', 'build-pattern-index.py')

spec = importlib.util.spec_from_file_location('build_pattern_index', SCRIPT)
bpi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bpi)

SVG = '<svg class="svg-preview" viewBox="0 0 10 10"></svg>'
OLD = b'// the previous index.js, with cover\nwindow.patternIndex = [];\n'
SLUGS = ('p1', 'p2')


def page_of(cmd):
    """The local path of the page Chrome was asked to open."""
    return urllib.request.url2pathname(urllib.parse.urlparse(cmd[-1]).path)


class BuildPatternIndex(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='bpi-test-')
        pats = os.path.join(self.tmp, 'assets', 'patterns')
        index = []
        for slug in SLUGS:
            os.makedirs(os.path.join(pats, slug))
            sweep = ['%s/x-%d.svg' % (slug, i) for i in range(6)]
            for rel in sweep + ['%s.svg' % slug]:
                with open(os.path.join(pats, rel.replace('/', os.sep)), 'w', encoding='utf-8') as f:
                    f.write(SVG)
            index.append({'slug': slug, 'title': slug, 'tags': ['GRID'], 'shapes': 1,
                          'file': '%s.svg' % slug, 'sweep': {'param': 'x', 'files': sweep}})
        with open(os.path.join(pats, 'index.json'), 'w', encoding='utf-8') as f:
            json.dump(index, f)
        self.dst = os.path.join(pats, 'index.js')
        with open(self.dst, 'wb') as f:
            f.write(OLD)
        patches = [
            mock.patch.object(bpi, 'PATTERNS', pats),
            mock.patch.object(bpi, 'SRC', os.path.join(pats, 'index.json')),
            mock.patch.object(bpi, 'DST', self.dst),
            mock.patch.object(sys, 'argv', ['build-pattern-index.py', '--chrome', 'stub-chrome']),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.calls = []

    def run_with(self, run):
        """main() with subprocess.run replaced by `run`; its stdout."""
        def recording(cmd, **kw):
            self.calls.append(cmd)
            return run(cmd, **kw)
        out = io.StringIO()
        with mock.patch.object(bpi.subprocess, 'run', recording), contextlib.redirect_stdout(out):
            bpi.main()
        return out.getvalue()

    def assert_fails_untouched(self, run, *words):
        with self.assertRaises(SystemExit) as cm:
            self.run_with(run)
        with open(self.dst, 'rb') as f:
            self.assertEqual(f.read(), OLD, 'index.js was rewritten after a failed measurement')
        self.assertEqual(sorted(os.listdir(os.path.dirname(self.dst))),
                         ['index.js', 'index.json', 'p1', 'p1.svg', 'p2', 'p2.svg'],
                         'something was left beside index.js')
        code = cm.exception.code
        self.assertNotIn(code, (0, None), 'exited 0')
        message = str(code)
        for w in words + ('not written',):
            self.assertIn(w, message)
        return message

    @staticmethod
    def answer(rc=0, stdout='', stderr=''):
        return lambda cmd, **kw: subprocess.CompletedProcess(cmd, rc, stdout, stderr)

    @staticmethod
    def covers(values):
        return '<html><body><pre id="out">COVER %s</pre></body></html>' % json.dumps(values)

    def test_no_chrome_found(self):
        with mock.patch.object(bpi, 'find_chrome', lambda explicit: None):
            self.assert_fails_untouched(self.answer(), 'no Chrome or Edge found')

    def test_chrome_cannot_start(self):
        def missing(cmd, **kw):
            raise FileNotFoundError(2, 'The system cannot find the file specified', cmd[0])
        self.assert_fails_untouched(missing, 'stub-chrome')

    def test_chrome_times_out(self):
        def slow(cmd, **kw):
            raise subprocess.TimeoutExpired(cmd, kw.get('timeout'))
        self.assert_fails_untouched(slow, 'did not finish')

    def test_chrome_crashes(self):
        self.assert_fails_untouched(self.answer(rc=3, stderr='[0925/1:FATAL] crashed\n'), 'code 3', 'crashed')

    def test_chrome_dumps_before_measuring(self):
        self.assert_fails_untouched(self.answer(stdout='<pre id="out">measuring</pre>'), 'before it finished')

    def test_chrome_writes_invalid_json(self):
        self.assert_fails_untouched(self.answer(stdout='<pre id="out">COVER {nope</pre>'), 'invalid JSON')

    def test_nothing_measured(self):
        self.assert_fails_untouched(self.answer(stdout=self.covers({'p1': None, 'p2': None})), 'could not measure any')

    def test_a_pattern_missing_from_the_answer(self):
        self.assert_fails_untouched(self.answer(stdout=self.covers({'p1': 0.1})), 'p2')

    def test_a_measurement_writes_cover(self):
        out = self.run_with(self.answer(stdout=self.covers({'p1': 0.1234, 'p2': 0.5})))
        with open(self.dst, encoding='utf-8') as f:
            text = f.read()
        self.assertTrue(text.startswith('// Generated by tools/build-pattern-index.py. Do not edit.\n'))
        index = json.loads(text.split('window.patternIndex = ', 1)[1].rstrip().rstrip(';'))
        self.assertEqual({p['slug']: p['cover'] for p in index}, {'p1': 0.1234, 'p2': 0.5})
        self.assertIn('1 over 0.30 lit: p2 50.0', out)

    def test_one_pattern_chrome_could_not_measure_is_null(self):
        self.run_with(self.answer(stdout=self.covers({'p1': 0.1, 'p2': None})))
        with open(self.dst, encoding='utf-8') as f:
            index = json.loads(f.read().split('window.patternIndex = ', 1)[1].rstrip().rstrip(';'))
        self.assertEqual({p['slug']: p['cover'] for p in index}, {'p1': 0.1, 'p2': None})

    def test_the_page_measures_the_new_index_without_touching_index_js(self):
        """The page Chrome opens loads the index being built from outside the
        tree, and patterns.js from res/visuals, so index.js is written once,
        after a measurement, and never before one."""
        seen = {}

        def look(cmd, **kw):
            with open(self.dst, 'rb') as f:
                seen['dst'] = f.read()
            with open(page_of(cmd), encoding='utf-8') as f:
                html = f.read()
            base = re.search(r'<base href="([^"]+)">', html).group(1)
            src = re.search(r'<script src="([^"]+/index\.js)"></script>', html).group(1)
            seen['base'] = urllib.request.url2pathname(urllib.parse.urlparse(base).path)
            with open(urllib.request.url2pathname(urllib.parse.urlparse(src).path), encoding='utf-8') as f:
                seen['index'] = f.read()
            seen['patterns.js'] = '<script src="patterns.js"></script>' in html
            return subprocess.CompletedProcess(cmd, 0, self.covers({'p1': 0.1, 'p2': 0.2}), '')

        self.run_with(look)
        self.assertEqual(seen['dst'], OLD, 'index.js was written before the measurement')
        self.assertEqual(os.path.normcase(os.path.abspath(seen['base'])), os.path.normcase(VISUALS))
        self.assertTrue(seen['patterns.js'])
        measured = json.loads(seen['index'].split('window.patternIndex = ', 1)[1].rstrip().rstrip(';'))
        self.assertEqual([p['slug'] for p in measured], list(SLUGS))
        self.assertTrue(all('cover' not in p for p in measured))
        self.assertFalse(os.path.exists(os.path.dirname(page_of(self.calls[0]))), 'the temp page was left behind')


if __name__ == '__main__':
    unittest.main()
