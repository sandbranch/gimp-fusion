#!/usr/bin/env python3
# Compares what GIMP made (tests/output/gimp/got, from tests/gimp-test.py)
# with fusion-cli's results (tests/output/gimp/expect): the plug-in must
# give what the core gives, to the rounding of the image's precision.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
import os
import sys

import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, 'ref'))
import pfm  # noqa: E402

g = os.path.join(here, 'output', 'gimp')
failures = 0
U8, U16 = 0.5 / 255 + 1e-6, 0.5 / 65535 + 1e-6


def check(ok, what):
    global failures
    print('FT %s %s' % ('PASS' if ok else 'FAIL', what), flush=True)
    failures += not ok


def got(name):
    return pfm.read(os.path.join(g, 'got', name + '.pfm'))


def exp(name):
    return pfm.read(os.path.join(g, 'expect', name + '.pfm'))


def same(gname, ename, tol, what):
    try:
        a, b = got(gname), exp(ename)
    except FileNotFoundError as e:
        check(False, '%s: %s missing' % (what, os.path.basename(e.filename)))
        return
    d = float(np.abs(a - b).max()) if a.shape == b.shape else float('inf')
    check(d <= tol, '%s = fusion-cli, largest difference %.3g (allowed %.3g)' % (what, d, tol))


same('exp-u8', 'exp-default-u8', U8, 'Merge Exposures, 8-bit image')
same('exp-float', 'exp-default', 1e-6, 'Merge Exposures, floating point image')
same('exp-float-noclip', 'exp-noclip', 1e-6, 'Merge Exposures without clipping, floating point')
same('exp-weights', 'exp-weights', 1e-6, 'Merge Exposures, exponents 2, 0.5, 1.5, width 0.3, '
     '5 levels')
same('exp-u16', 'exp16-default', U16, 'Merge Exposures, 16-bit image')
same('exp-sel', 'exp-sel-u8', U8, 'Merge Exposures of 3 of 5 layers, selected out of order')
same('exp-group', 'exp-default-u8', U8, 'Merge Exposures of the layers inside a group')
same('exp-aligned', 'expmoved-aligned', U8, 'Merge Exposures with alignment, 8-bit')
same('micro-pyramid', 'micro-pyramid', U16, 'Focus Stack, 16-bit linear gray')
same('micro-pixels', 'micro-pixels', 1e-7, 'Focus Stack, pixels method: the layers\' own '
     'values')
same('micro-soft', 'micro-soft', U16, 'Focus Stack, weighted by sharpness, power 3')
same('focus-coeff', 'focus-coeff', U8, 'Focus Stack, coefficients, 8-bit RGB')
same('focus-default', 'focus-default', U8, 'Focus Stack, 8-bit RGB')

try:
    d = got('micro-depth')
    e = exp('micro-pyramid-depth')
    n = 9
    ok = e >= 0
    diff = float(np.abs(d[ok] - e[ok] / (n - 1)).max())
    check(diff <= U16, 'the depth map layer = fusion-cli\'s depth / (layers - 1), largest '
          'difference %.3g' % diff)
    truth = pfm.read(os.path.join(g, 'in', 'micro-depth.pfm')) / (n - 1)
    cells = pfm.read(os.path.join(g, 'in', 'micro-sharp.pfm')) > 0.05
    r = float(np.corrcoef(d[cells], truth[cells])[0, 1])
    check(r > 0.95, 'the depth map correlates with the true depth on the cells: %.4f' % r)
except FileNotFoundError:
    check(False, 'the depth map layer was written')

try:
    lin, per = got('exp-linear'), got('exp-float')
    check(np.abs(lin - per).max() > 0.01 and lin.min() >= 0 and lin.max() <= 1,
          'blending in linear light gives another result, in range (largest difference '
          '%.3f from perceptual)' % np.abs(lin - per).max())
except FileNotFoundError:
    check(False, 'the linear result was written')

try:
    a = got('exp-offset-alpha')
    c = got('exp-offset')
    ref = exp('exp-cropped')
    p = 10 * np.log10(1 / np.mean((c[:, 12:] - ref) ** 2))
    check(a[:, :12].max() == 0 and a[:, 12:].min() == 1,
          'layers shifted by 12 pixels: the uncovered strip is transparent, the rest opaque')
    check(p > 35, 'there, the result matches the fusion of the uncropped frames (%.1f dB): '
          'no darkening from the empty strip' % p)
except FileNotFoundError:
    check(False, 'the shifted layers\' result was written')

try:
    m = got('exp-mask')
    ref = exp('exp-4-l3')
    d = float(np.abs(m[:, :60] - ref[:, :60]).max())
    check(d < 1e-4, 'a layer mask that hides the brightest layer on the left: there the '
          'result is the fusion of the other four (3 levels, largest difference %.2g)' % d)
except FileNotFoundError:
    check(False, 'the masked result was written')

try:
    w, h = map(int, open(os.path.join(g, 'got', 'crop.txt')).read().split())
    check(300 - 8 <= w < 300 and 200 - 8 <= h < 200,
          'Crop to the common area: %d x %d, the frames move by up to 3.2 and 2.6 pixels' % (w, h))
except FileNotFoundError:
    check(False, 'the crop size was written')

# Align Layers by Content: fusion-cli finds no shift left between the layers
try:
    lines = open(os.path.join(g, 'realign.txt')).read().split()
    vals = np.array(lines, float).reshape(-1, 6)
    worst = float(np.hypot(vals[:, 3], vals[:, 4]).max())
    check(worst < 0.15, 'Align Layers by Content: afterwards the layers are %.3f pixels apart '
          'at most (fusion-cli align on GIMP\'s result)' % worst)
except (FileNotFoundError, ValueError):
    check(False, 'the aligned layers were checked')

print('FT compare failures: %d' % failures)
sys.exit(1 if failures else 0)
