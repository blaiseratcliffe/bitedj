#!/usr/bin/env python3
"""Convert a clip into the form the visuals page plays, in assets/video/.

The Pi 5 has no hardware H.264 decoder, so Chromium decodes video on the CPU
beside Mixxx's audio engine. Every clip is converted before use, and this
script is the conversion:

    640x360     below the 960x540 render size on purpose; the GPU scales it
                up. On 2026-09-23 bitepi played four decks with four
                waveforms while a 960x540 clip was on screen, and Mixxx logged
                4 audio underruns in about 2.5 minutes against none in about
                4 minutes without video, so the decode cost has to come down.
                640x360 is 44 percent of the pixels. Every clip is scaled to
                cover 640x360 with its own proportions and the excess is
                cropped off equally from both sides, so a 3:2 or 4:3 source
                fills the frame undistorted instead of being stretched; for a
                16:9 source the crop removes nothing.
    24 fps at most
                for the same reason: a 30 or 60 fps source is brought down to
                24 with ffmpeg's -fpsmax, and a slower one keeps its own rate.
                The page renders at a 30 fps cap (FPS in director.js), so 24
                is still close to every frame it shows.
    no audio    the page is muted and never reads it.
    H.264 main  main rather than high: high's 8x8 transform is more work per
                frame for a software decoder.
    a keyframe every 2 s
                video.js starts each clip at a random point, and a seek lands
                on the keyframe before the target and decodes forward from
                there, so the keyframe interval is the worst case of that wait.
    +faststart  the index (moov) goes at the front of the file, so playback
                can start before the whole file has been read.

Run from res/visuals, then rebuild the index:

    python tools/prepare-video.py <input> [--name NAME] [--ffmpeg PATH]
    python tools/build-video-index.py

ffmpeg is taken from PATH unless --ffmpeg names it. The output is
assets/video/<slug>.mp4, where the slug is NAME if given and the input's
file name if not, reduced to lower case ASCII letters, digits and hyphens and
cut at a hyphen to at most 40 characters. Accented letters keep their base
letter; spaces, punctuation and emoji become hyphens. A YouTube download's
name can carry all of those, and the slug is what the index and the page's
URLs use.

Nothing in assets/video/ is committed. Clips are usually someone else's work,
so they are deployed from the working tree by deploy-skin.sh, which rsyncs
res/, as the Book of Shapes patterns are.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VIDEO = os.path.join(ROOT, 'assets', 'video')

WIDTH, HEIGHT = 640, 360
FPS_MAX = 24
KEYFRAME_S = 2


SLUG_MAX = 40


def slug(text):
    ascii_text = unicodedata.normalize('NFKD', text).encode('ascii', 'ignore').decode('ascii')
    s = re.sub(r'[^a-z0-9]+', '-', ascii_text.lower()).strip('-')
    if len(s) > SLUG_MAX:
        cut = s[:SLUG_MAX + 1]
        s = cut[:cut.rfind('-')] if '-' in cut else s[:SLUG_MAX]
        s = s.strip('-')
    return s or 'clip'


def build_command(ffmpeg, src, dst, input_args=(), extra_args=()):
    """The ffmpeg command for one conversion, as a list.

    ffmpeg is the executable, or a list that starts the command (the Pi's
    admin service passes [python, stub_ffmpeg.py] in its tests). input_args
    go just before -i: the service's camera stand-in puts its v4l2 options
    there. extra_args go just before the output: the service adds -threads
    and -progress there. With neither, this is the command main() runs.
    pi/bin/bitedj-visuals-admin imports this function from the deployed copy
    of this file, so the PC and the Pi convert with the same settings.
    """
    prefix = list(ffmpeg) if isinstance(ffmpeg, (list, tuple)) else [ffmpeg]
    scale = ('scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d,setsar=1'
             % (WIDTH, HEIGHT, WIDTH, HEIGHT))
    return prefix + ['-hide_banner', '-y'] + list(input_args) + [
        '-i', src,
        '-an', '-sn', '-dn',
        '-vf', scale,
        '-fpsmax', str(FPS_MAX),
        '-c:v', 'libx264', '-profile:v', 'main', '-pix_fmt', 'yuv420p',
        '-preset', 'medium', '-crf', '23',
        '-force_key_frames', 'expr:gte(t,n_forced*%d)' % KEYFRAME_S,
        '-movflags', '+faststart',
    ] + list(extra_args) + [dst]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('input', help='the clip to convert')
    ap.add_argument('--name', help='output name without extension')
    ap.add_argument('--ffmpeg', default='ffmpeg', help='ffmpeg executable (default: from PATH)')
    args = ap.parse_args()

    ffmpeg = shutil.which(args.ffmpeg) or (args.ffmpeg if os.path.exists(args.ffmpeg) else None)
    if not ffmpeg:
        sys.exit('no ffmpeg at %r; put it on PATH or pass --ffmpeg' % args.ffmpeg)
    if not os.path.exists(args.input):
        sys.exit('no such file: %s' % args.input)

    os.makedirs(VIDEO, exist_ok=True)
    name = args.name or os.path.splitext(os.path.basename(args.input))[0]
    dst = os.path.join(VIDEO, slug(name) + '.mp4')
    cmd = build_command(ffmpeg, args.input, dst)
    # Escaped, because a Windows console's code page cannot print an emoji
    # and the input's name is the one part of the command that may hold one.
    line = ' '.join('"%s"' % c if ' ' in c else c for c in cmd)
    print(line.encode('ascii', 'backslashreplace').decode('ascii'), flush=True)
    result = subprocess.run(cmd)
    if result.returncode != 0:
        sys.exit('ffmpeg failed with exit code %d' % result.returncode)
    print('%s, %.1f MB' % (dst, os.path.getsize(dst) / 1e6))
    print('now run tools/build-video-index.py')


if __name__ == '__main__':
    main()
