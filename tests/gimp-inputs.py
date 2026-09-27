#!/usr/bin/env python3
# Writes the inputs of tests/gimp-test.py (PFM files in tests/output/gimp/in)
# and a shell script of fusion-cli runs that make the expected results
# (tests/output/gimp/expect), which tests/gimp-test.sh runs in the SDK.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
import os
import shlex
import sys

import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, 'ref'))
import pfm    # noqa: E402
import synth  # noqa: E402

g = os.path.join(here, 'output', 'gimp')
inp, exp = os.path.join(g, 'in'), os.path.join(g, 'expect')
os.makedirs(inp, exist_ok=True)
os.makedirs(exp, exist_ok=True)
cli = sys.argv[1]
jobs = []


def stack(name, frames):
    paths = []
    for i, f in enumerate(frames):
        p = os.path.join(inp, '%s-%02d.pfm' % (name, i))
        pfm.write(p, f)
        paths.append(p)
    return paths


def gimp_in(tag, count):
    """the layers as GIMP read them (tests/gimp-test.py writes them)"""
    return [os.path.join(g, 'got', 'in-%s-%02d.pfm' % (tag, i)) for i in range(count)]


def job(out, *args, depth=None):
    a = [cli] + list(args) + ['-o', os.path.join(exp, out + '.pfm')]
    if depth:
        a.insert(2, '--depth=' + os.path.join(exp, depth + '.pfm'))
    jobs.append(' '.join(shlex.quote(x) for x in a))


rad = synth.hdr_scene(200, 300, seed=11)
evs = [-4, -2, 0, 2, 4]
e8 = stack('exp', synth.exposures(rad, evs, 8))
job('exp-default', 'exposures', *e8)
u8 = gimp_in('u8', 5)
job('exp-default-u8', 'exposures', *u8)
job('exp-sel-u8', 'exposures', u8[1], u8[2], u8[4])
job('exp-noclip', 'exposures', '--no-clip', *e8)
job('exp-sel', 'exposures', e8[1], e8[2], e8[4])
job('exp-weights', 'exposures', '--contrast=2', '--saturation=0.5', '--exposure=1.5',
    '--sigma=0.3', '--levels=5', *e8)
stack('expcrop', [pfm.read(p)[:, :-12] for p in e8])
e16 = stack('exp16', synth.exposures(rad, evs[1:4], 16))
job('exp-4-l3', 'exposures', '--levels=3', *e8[:4])
job('exp-cropped', 'exposures', *[os.path.join(inp, 'expcrop-%02d.pfm' % i) for i in range(5)])
job('exp16-default', 'exposures', *gimp_in('u16', 3))
motion = [(1.0, 0.0, -3.2, 1.9), (1.0, 0.0, 1.1, -0.7), (1.0, 0.0, 0.0, 0.0),
          (1.0, 0.0, 2.3, 1.2), (1.0, 0.0, -1.4, -2.6)]
em = stack('expmoved', synth.exposures(rad, evs, 8, motion=motion))
job('expmoved-aligned', 'exposures', '--align=translation', *gimp_in('moved-u8', 5))
with open(os.path.join(g, 'motion.txt'), 'w') as f:
    for m in motion:
        f.write('%r %r %r %r\n' % m)

sharp, depth, slices, mmotion = synth.microscopy_stack(160, 192, 9, magnification=0.0,
                                                       drift=(0.0, 0.0))
ms = stack('micro', slices)
pfm.write(os.path.join(inp, 'micro-sharp.pfm'), sharp)
pfm.write(os.path.join(inp, 'micro-depth.pfm'), depth)
mg = gimp_in('micro', 9)
job('micro-pyramid', 'focus', '--method=pyramid', *mg, depth='micro-pyramid-depth')
job('micro-pixels', 'focus', '--method=pixels', *mg)
job('micro-soft', 'focus', '--method=soft', '--power=3', *mg)

h, w, n = 150, 220, 6
fsharp = synth.photo_texture(h, w, seed=12)
fdepth = synth.depth_map(h, w, n, seed=13)
fs = stack('focus', synth.focus_stack(fsharp, fdepth, n, 1.2, bits=8))
pfm.write(os.path.join(inp, 'focus-sharp.pfm'), fsharp)
fg = gimp_in('focus-u8', 6)
job('focus-coeff', 'focus', '--method=coeff', *fg)
job('focus-default', 'focus', *fg)

with open(os.path.join(g, 'expect.sh'), 'w') as f:
    f.write('set -e\n' + '\n'.join(jobs) + '\n')
print('%d expected results to make' % len(jobs))
