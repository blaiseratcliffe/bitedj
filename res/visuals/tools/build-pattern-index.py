#!/usr/bin/env python3
"""Turn assets/patterns/index.json into assets/patterns/index.js.

The visuals page runs from file:// on the Pi, where fetch() of a file URL is
refused whatever flags Chromium was given. A <script> tag is not, so the index
travels as a script that assigns window.patternIndex rather than as JSON the
page would have to read. Run this once after the pattern assets are (re)synced:

    python tools/build-pattern-index.py            # from res/visuals

Neither index.json nor index.js is committed. assets/patterns/ is git-ignored
because the pattern licence allows on-screen use but not redistribution; this
script is the committed half of that arrangement, so a fresh checkout plus the
asset drop reproduces the index exactly.

Three things this adds to what index.json already says, all of them measured
from the files rather than assumed:

  wire    true when the artwork carries --occlusion-color, i.e. it is a solid
          3D form whose faces hide the edges behind them. patterns.js renders
          those with black faces and white strokes; everything else gets white
          faces and white strokes. Recolouring by alpha instead turns every
          occluded form into a white blob, which is what the first pass did.
  bytes   the total size of the seven frames, and
  frame   the size of the largest one of them, which is the number patterns.js
          actually gates on: a pattern rasterises one frame per animation
          frame, so the total is spread over seven frames and the largest
          single frame is the stall. The `shapes` count in index.json is for
          the default frame only and the sweep can be five times heavier.
  frames  the seven file paths, sweep 0 to 5 then the default, with any
          degenerate frame replaced by its nearest usable neighbour. Eight
          patterns have a sweep frame 0 that is not artwork at all: with the
          slider at its minimum the site emits a 1591 byte paperclip icon,
          which would land on the screen as a paperclip. Three of the eight
          never reach the screen anyway, iso-sphere because patterns.js keeps
          it out on size and backpack-grid and masked_letter_grid because it
          keeps them out on coverage, so five of them are on screen with a
          flattened sweep end.
  cover   the default frame's lit fraction, 0 to 1 rounded to four decimals,
          or null when Chrome could not measure it. The service cannot
          rasterise SVG, so this script measures it instead, in headless
          Chrome, with the page's own code (patterns.measureCover(), which
          calls the same svgDoc() rewrite, the same SIZE canvas on black and
          the same coverage() the page's own loader uses). The admin service
          leaves out of its library any pattern whose cover is over COVER_MAX
          (Decision 34, plan task A21): patterns.js already leaves such a
          pattern out of the rotation, and without this the service still
          listed it, let a Random set tick it, and counted it toward the
          set's minimum, so a set could pass validation on a pattern the TV
          never draws. This step needs Chrome or Edge on the PC: pass
          --chrome PATH, set $CHROME, or install one of Google Chrome or
          Microsoft Edge at their usual Windows locations.

index.js is written once, after the measurement, or not at all. An index
without cover hides nothing (Decision 34), so writing one when Chrome is
missing, crashes, times out or dumps an unfinished page would put every
too-solid pattern back in the library on the next deploy. On any of those the
script exits non-zero and leaves the previous index.js exactly as it was.
Chrome measures a copy of tools/measure-cover.html in a temp directory that
loads the index being built from there, so the real one is never a draft.
"""

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PATTERNS = os.path.join(ROOT, 'assets', 'patterns')
SRC = os.path.join(PATTERNS, 'index.json')
DST = os.path.join(PATTERNS, 'index.js')
MEASURE_PAGE = os.path.join(HERE, 'measure-cover.html')

# Every real pattern export carries this class on its root <svg>. The
# placeholder icon does not, which is the only reliable way to tell them apart:
# the icon is also valid SVG and also has a viewBox.
MARK = 'svg-preview'

# The same ceiling patterns.js gates the rotation on (COVER_MAX,
# bitedj/res/visuals/patterns.js:501); only used here to print a summary.
COVER_MAX = 0.30

# Chrome/Edge candidates on a Windows dev PC, tried after --chrome and $CHROME.
CHROME_PATHS = (
    'C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
)
CHROME_NAMES = ('google-chrome', 'chromium', 'chromium-browser')

OUT_RE = re.compile(r'<pre id="out">(.*?)</pre>', re.S)

# What the temp copy of measure-cover.html changes: its base, so patterns.js
# and the frames still resolve from res/visuals/, and the index it loads.
PAGE_BASE = '<base href="../">'
PAGE_INDEX = 'src="assets/patterns/index.js"'
CHROME_TIMEOUT_S = 300


class MeasureError(Exception):
    """The cover measurement failed; the message says how."""


def read_head(path, n=4096):
    with open(path, encoding='utf-8') as f:
        return f.read(n)


def find_chrome(explicit):
    """--chrome, else $CHROME, else the first of the usual install paths or
    PATH names that exists. None when nothing was found."""
    if explicit:
        return explicit
    if os.environ.get('CHROME'):
        return os.environ['CHROME']
    for path in CHROME_PATHS:
        if os.path.exists(path):
            return path
    for name in CHROME_NAMES:
        found = shutil.which(name)
        if found:
            return found
    return None


def write_index(path, out):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// Generated by tools/build-pattern-index.py. Do not edit.\n')
        f.write('window.patternIndex = %s;\n' % json.dumps(out, separators=(',', ':')))


def measure_covers(chrome, out):
    """{slug: cover or None} for every entry of `out`, read from the
    "COVER {...}" tools/measure-cover.html writes into its <pre id="out"> once
    every pattern in the index it loaded has been measured (Decision 34).

    Chrome opens a copy of that page in a temp directory, beside `out` written
    as an index script, so nothing under assets/patterns/ is written before
    the measurement has worked. A fresh --user-data-dir every run, so this
    never attaches to a Chrome the person has open. Raises MeasureError on
    every way it can fail, including a slug the page did not answer for and
    an answer with no figure at all, which is a page that measured nothing."""
    with open(MEASURE_PAGE, encoding='utf-8') as f:
        page = f.read()
    for needle in (PAGE_BASE, PAGE_INDEX):
        if page.count(needle) != 1:
            raise MeasureError('tools/measure-cover.html no longer contains %s exactly once' % needle)
    tmp = tempfile.mkdtemp(prefix='bitedj-measure-cover-')
    try:
        index_js = os.path.join(tmp, 'index.js')
        write_index(index_js, out)
        page = page.replace(PAGE_BASE, '<base href="%s/">' % pathlib.Path(ROOT).as_uri())
        page = page.replace(PAGE_INDEX, 'src="%s"' % pathlib.Path(index_js).as_uri())
        page_path = os.path.join(tmp, 'measure-cover.html')
        with open(page_path, 'w', encoding='utf-8') as f:
            f.write(page)
        try:
            r = subprocess.run([
                chrome, '--headless=new', '--disable-gpu',
                '--allow-file-access-from-files',
                '--user-data-dir=' + os.path.join(tmp, 'profile'),
                '--virtual-time-budget=120000',
                '--dump-dom', pathlib.Path(page_path).as_uri(),
            ], capture_output=True, text=True, timeout=CHROME_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            raise MeasureError('Chrome did not finish within %d s' % CHROME_TIMEOUT_S)
        except OSError as e:
            raise MeasureError('could not start %s: %s' % (chrome, e))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if r.returncode != 0:
        tail = (r.stderr or '').strip().splitlines()[-1:] or ['no output on stderr']
        raise MeasureError('Chrome exited with code %d: %s' % (r.returncode, tail[0][:200]))
    m = OUT_RE.search(r.stdout or '')
    text = m.group(1).strip() if m else ''
    if text == 'measuring' or not text:
        raise MeasureError('Chrome dumped tools/measure-cover.html before it finished measuring; '
                           'try again or raise --virtual-time-budget')
    if not text.startswith('COVER '):
        raise MeasureError('tools/measure-cover.html wrote something unexpected: %s' % text[:200])
    try:
        covers = json.loads(text[len('COVER '):])
    except ValueError as e:
        raise MeasureError('tools/measure-cover.html wrote invalid JSON: %s' % e)
    if not isinstance(covers, dict):
        raise MeasureError('tools/measure-cover.html wrote %s, not an object' % type(covers).__name__)
    missing = [p['slug'] for p in out if p['slug'] not in covers]
    if missing:
        raise MeasureError('tools/measure-cover.html gave no figure for %s' % ', '.join(missing))
    if out and all(covers[p['slug']] is None for p in out):
        raise MeasureError('Chrome could not measure any pattern')
    return covers


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--chrome', help='path to a Chrome or Edge binary; '
                        'else $CHROME, else the usual install paths')
    opts = parser.parse_args()

    if not os.path.exists(SRC):
        sys.exit('no %s; the pattern assets are not in place' % SRC)
    with open(SRC, encoding='utf-8') as f:
        index = json.load(f)

    out = []
    dropped = []
    patched = 0
    for entry in index:
        slug = entry['slug']
        paths = list(entry['sweep']['files']) + [entry['file']]
        if len(paths) != 7:
            dropped.append((slug, 'expected 6 sweep frames, found %d' % (len(paths) - 1)))
            continue

        good = []
        for rel in paths:
            full = os.path.join(PATTERNS, rel.replace('/', os.sep))
            good.append(os.path.exists(full) and MARK in read_head(full))
        if not any(good):
            dropped.append((slug, 'no usable frame'))
            continue

        # Replace a degenerate frame with the nearest usable one, so the sweep
        # is still six frames and the morph simply flattens at that end.
        frames = []
        for i, rel in enumerate(paths):
            if good[i]:
                frames.append(rel)
                continue
            near = min((j for j in range(7) if good[j]), key=lambda j: abs(j - i))
            frames.append(paths[near])
            patched += 1

        total = 0
        biggest = 0
        for rel in set(frames):
            size = os.path.getsize(os.path.join(PATTERNS, rel.replace('/', os.sep)))
            total += size
            biggest = max(biggest, size)

        default_head = read_head(os.path.join(PATTERNS, entry['file'].replace('/', os.sep)))
        out.append({
            'slug': slug,
            'title': entry.get('title', slug),
            'tags': entry.get('tags', []),
            'shapes': entry.get('shapes', 0),
            'bytes': total,
            'frame': biggest,
            'wire': '--occlusion-color' in default_head,
            'param': entry.get('sweep', {}).get('param', ''),
            'frames': frames,
        })

    print('%d patterns, %d frames patched, %d dropped'
          % (len(out), patched, len(dropped)))
    for slug, why in dropped:
        print('  dropped %s: %s' % (slug, why))
    wire = sum(1 for p in out if p['wire'])
    print('  %d wire (occluded faces), %d solid' % (wire, len(out) - wire))
    heavy = sorted(out, key=lambda p: -p['frame'])[:6]
    print('  biggest single frames: %s'
          % ', '.join('%s %.2f MB' % (p['slug'], p['frame'] / 1e6) for p in heavy))

    # Nothing is written unless this works; see the module docstring.
    unchanged = ('%s was not written, and the one there is unchanged; an index without '
                 '"cover" would un-hide every too-solid pattern (Decision 34)' % DST)
    chrome = find_chrome(opts.chrome)
    if not chrome:
        sys.exit('no Chrome or Edge found; pass --chrome PATH or set $CHROME.\n' + unchanged)
    try:
        covers = measure_covers(chrome, out)
    except MeasureError as e:
        sys.exit('could not measure cover: %s.\n%s' % (e, unchanged))
    for entry in out:
        entry['cover'] = covers[entry['slug']]

    # A temp file renamed over the old one, so an interrupted write cannot
    # leave half an index behind either.
    tmp = DST + '.tmp'
    write_index(tmp, out)
    os.replace(tmp, DST)
    print('  -> %s' % DST)

    over = sorted((p for p in out if isinstance(p['cover'], (int, float)) and p['cover'] > COVER_MAX),
                  key=lambda p: -p['cover'])
    nulls = [p['slug'] for p in out if p['cover'] is None]
    print('  %d over %.2f lit: %s' % (len(over), COVER_MAX,
          ', '.join('%s %.1f' % (p['slug'], 100 * p['cover']) for p in over) or 'none'))
    if nulls:
        print('  could not measure: %s' % ', '.join(nulls))


if __name__ == '__main__':
    main()
