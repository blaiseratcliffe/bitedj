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


def measure_covers(chrome):
    """{slug: cover or None}, read from the "COVER {...}" tools/measure-cover.html
    writes into its <pre id="out"> once every pattern in the index it loaded
    has been measured (Decision 34). A fresh --user-data-dir every run, so
    this never attaches to a Chrome the person has open."""
    url = pathlib.Path(MEASURE_PAGE).as_uri()
    user_data_dir = tempfile.mkdtemp(prefix='bitedj-measure-cover-')
    try:
        r = subprocess.run([
            chrome, '--headless=new', '--disable-gpu',
            '--allow-file-access-from-files',
            '--user-data-dir=' + user_data_dir,
            '--virtual-time-budget=120000',
            '--dump-dom', url,
        ], capture_output=True, text=True, timeout=300)
    finally:
        shutil.rmtree(user_data_dir, ignore_errors=True)
    m = OUT_RE.search(r.stdout)
    text = m.group(1).strip() if m else ''
    if text == 'measuring' or not text:
        sys.exit('Chrome dumped tools/measure-cover.html before it finished measuring; '
                  'try again or raise --virtual-time-budget')
    if not text.startswith('COVER '):
        sys.exit('tools/measure-cover.html wrote something unexpected: %s' % text[:200])
    try:
        return json.loads(text[len('COVER '):])
    except ValueError as e:
        sys.exit('tools/measure-cover.html wrote invalid JSON: %s' % e)


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

    body = json.dumps(out, separators=(',', ':'))
    with open(DST, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// Generated by tools/build-pattern-index.py. Do not edit.\n')
        f.write('window.patternIndex = %s;\n' % body)

    print('%d patterns, %d frames patched, %d dropped -> %s'
          % (len(out), patched, len(dropped), DST))
    for slug, why in dropped:
        print('  dropped %s: %s' % (slug, why))
    wire = sum(1 for p in out if p['wire'])
    print('  %d wire (occluded faces), %d solid' % (wire, len(out) - wire))
    heavy = sorted(out, key=lambda p: -p['frame'])[:6]
    print('  biggest single frames: %s'
          % ', '.join('%s %.2f MB' % (p['slug'], p['frame'] / 1e6) for p in heavy))

    chrome = find_chrome(opts.chrome)
    if not chrome:
        sys.exit('no Chrome or Edge found; pass --chrome PATH or set $CHROME '
                  '(index.js was written without "cover")')
    covers = measure_covers(chrome)
    for entry in out:
        entry['cover'] = covers.get(entry['slug'])

    body = json.dumps(out, separators=(',', ':'))
    with open(DST, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// Generated by tools/build-pattern-index.py. Do not edit.\n')
        f.write('window.patternIndex = %s;\n' % body)

    over = sorted((p for p in out if isinstance(p['cover'], (int, float)) and p['cover'] > COVER_MAX),
                  key=lambda p: -p['cover'])
    nulls = [p['slug'] for p in out if p['cover'] is None]
    print('  %d over %.2f lit: %s' % (len(over), COVER_MAX,
          ', '.join('%s %.1f' % (p['slug'], 100 * p['cover']) for p in over) or 'none'))
    if nulls:
        print('  could not measure: %s' % ', '.join(nulls))


if __name__ == '__main__':
    main()
