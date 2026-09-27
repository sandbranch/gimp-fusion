#!/usr/bin/env python3
# Measures the core (tools/fusion-cli) against references and ground truth:
#
#   exposures  against a literal numpy port of Mertens' own Matlab code
#              (tests/ref/mertens.py), several settings, 8 and 16 bits,
#              gray, odd sizes; and against OpenCV's MergeMertens when
#              tests/output/venv has OpenCV (tests/crosscheck.sh sets that
#              up, checksum pinned)
#   focus      synthetic stacks with a known sharp image and depth map
#              (tests/ref/synth.py): PSNR against the sharp image for each
#              method, depth map correlation and error
#   alignment  frames moved by known shifts, rotations and scales:
#              the error at the corners, and fusion after alignment
#
# fusion-cli is FUSION_CLI_EXE; all runs go through one shell started by
# the command in FUSION_RUN (tests/crosscheck.sh: gimp-build.sh, in the SDK). Prints "XC PASS ..." or
# "XC FAIL ..." per check and the numbers into tests/output/crosscheck.json.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
import shlex
import subprocess
import sys

import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, 'ref'))
import mertens  # noqa: E402
import pfm      # noqa: E402
import synth    # noqa: E402

out = os.path.join(here, 'output', 'cross')
os.makedirs(out, exist_ok=True)
failures = 0
results = {}
jobs = []   # (label, argv) run in one go through the SDK


def check(ok, what):
    global failures
    print('XC %s %s' % ('PASS' if ok else 'FAIL', what), flush=True)
    failures += not ok


def psnr(a, b, crop=0):
    if crop:
        a, b = a[crop:-crop, crop:-crop], b[crop:-crop, crop:-crop]
    mse = np.mean((np.asarray(a, np.float64) - b) ** 2)
    return 10 * np.log10(1 / mse) if mse > 0 else 999.0


def path(name):
    return os.path.join(out, name + '.pfm')


def save_stack(name, frames):
    paths = []
    for i, f in enumerate(frames):
        p = path('%s-%02d' % (name, i))
        pfm.write(p, f)
        paths.append(p)
    return paths


def run_all():
    """runs the queued fusion-cli calls in one shell (one start of the SDK)"""
    script = os.path.join(out, 'jobs.sh')
    with open(script, 'w') as f:
        f.write('set -u\n')
        for label, argv in jobs:
            f.write('echo "== %s"\n' % label)
            f.write(' '.join(shlex.quote(a) for a in argv) + ' || echo "JOB FAILED %s"\n' % label)
    r = subprocess.run(shlex.split(os.environ.get('FUSION_RUN', '')) + ['sh %s' % shlex.quote(script)]
                       if os.environ.get('FUSION_RUN') else ['sh', script],
                       capture_output=True, text=True)
    log = r.stdout + r.stderr
    open(os.path.join(out, 'jobs.log'), 'w').write(log)
    jobs.clear()
    return log


def queue(label, *args):
    exe = os.environ.get('FUSION_CLI_EXE', os.path.join(here, 'output', 'build', 'fusion-cli'))
    jobs.append((label, [exe] + [str(a) for a in args]))


def transforms_from_log(log, label):
    lines = log.split('== ' + label + '\n', 1)[1].split('\n== ', 1)[0].strip().splitlines()
    t = {}
    for ln in lines:
        p = ln.split()
        if len(p) == 6 and p[0].isdigit():
            t[int(p[0])] = tuple(map(float, p[1:5])) + (float(p[5]),)
    return t


def invert(t):
    a, b, tx, ty = t[:4]
    d = a * a + b * b
    ia, ib = a / d, -b / d
    return (ia, ib, -(ia * tx - ib * ty), -(ib * tx + ia * ty))


def corner_error(t, u, w, h):
    cx, cy = (w - 1) / 2, (h - 1) / 2
    m = 0.0
    for x, y in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)):
        def ap(s):
            dx, dy = x - cx, y - cy
            return cx + s[0] * dx - s[1] * dy + s[2], cy + s[1] * dx + s[0] * dy + s[3]
        p, q = ap(t), ap(u)
        m = max(m, np.hypot(p[0] - q[0], p[1] - q[1]))
    return m


# ------------------------------------------------------------ exposures

rad = synth.hdr_scene(320, 480)
evs = [-4, -2, 0, 2, 4]
fr8 = synth.exposures(rad, evs, 8)
fr16 = synth.exposures(rad, evs[1:4], 16)
gray = [f @ np.array([0.2126, 0.7152, 0.0722]) for f in synth.exposures(rad, [-3, 0, 3], 8)]
odd_rad = synth.hdr_scene(157, 301, seed=7)
odd = synth.exposures(odd_rad, [-3, -1, 1, 3], 8)
p8 = save_stack('exp8', fr8)
p16 = save_stack('exp16', fr16)
pg = save_stack('expgray', gray)
podd = save_stack('expodd', odd)

cases = [
    ('default weights, 5 frames, 8 bits', p8, fr8, (1, 1, 1), 0.2, 0, []),
    ('contrast only', p8, fr8, (1, 0, 0), 0.2, 0, ['--saturation=0', '--exposure=0']),
    ('saturation and exposure only', p8, fr8, (0, 1, 1), 0.2, 0, ['--contrast=0']),
    ('exponents 2, 1, 0.5, sigma 0.3, 4 levels', p8, fr8, (2, 1, 0.5), 0.3, 4,
     ['--contrast=2', '--exposure=0.5', '--sigma=0.3', '--levels=4']),
    ('3 frames, 16 bits', p16, fr16, (1, 1, 1), 0.2, 0, []),
    ('gray, 3 frames', pg, gray, (1, 1, 1), 0.2, 0, []),
    ('odd size 301 x 157, 4 frames', podd, odd, (1, 1, 1), 0.2, 0, []),
]
for i, (label, paths, frames, m, sig, lev, opts) in enumerate(cases):
    queue('exp%d' % i, 'exposures', '--no-clip', *opts, '-o', path('exp-out-%d' % i), *paths)
queue('exp-cv', 'exposures', '--no-clip', '--levels=%d' % (mertens.levels(320, 480) + 1),
      '-o', path('exp-out-cv'), *p8)
queue('exp-clip', 'exposures', '-o', path('exp-out-clip'), *p8)

# OpenCV input is 8 bit; the same frames
# ---------------------------------------------------------------- focus

h, w, n = 240, 360, 8
sharp = synth.photo_texture(h, w)
depth = synth.depth_map(h, w, n)
stack = synth.focus_stack(sharp, depth, n, 1.2)
pf = save_stack('focus', stack)
noisy = synth.focus_stack(sharp, depth, n, 1.2, noise=0.01, bits=8)
pn = save_stack('focusnoisy', noisy)
methods = ['pyramid', 'coeff', 'soft', 'pixels']
for m in methods:
    queue('focus-' + m, 'focus', '--method=' + m, '--depth=' + path('focus-depth-' + m),
          '-o', path('focus-out-' + m), *pf)
    queue('focusn-' + m, 'focus', '--method=' + m, '--depth=' + path('focusn-depth-' + m),
          '-o', path('focusn-out-' + m), *pn)
queue('focus-variance', 'focus', '--measure=variance', '--depth=' + path('focus-depth-var'),
      '-o', path('focus-out-var'), *pf)

# microscopy: 16-bit gray, magnification and drift
msharp, mdepth, mslices, mmotion = synth.microscopy_stack(256, 256, 12)
pm = save_stack('micro', mslices)
queue('micro-noalign', 'focus', '--depth=' + path('micro-depth-none'), '-o',
      path('micro-out-none'), *pm)
queue('micro-align', 'focus', '--align=scale', '--depth=' + path('micro-depth-scale'), '-o',
      path('micro-out-scale'), *pm)
queue('micro-transforms', 'align', '--model=scale', *pm)

# ------------------------------------------------------------ alignment

motion = [(1.0, 0.0, -3.7, 2.2), (np.cos(0.004), np.sin(0.004), 1.3, -0.6), (1.0, 0.0, 0.0, 0.0),
          (1.003, 0.0, 2.4, 1.1), (np.cos(-0.006), np.sin(-0.006), -1.2, -4.5)]
moved = synth.exposures(rad, evs, 8, motion=motion)
pmv = save_stack('expmoved', moved)
queue('exp-transforms', 'align', '--model=similarity', *pmv)
queue('exp-aligned', 'exposures', '--align=similarity', '-o', path('exp-out-aligned'), *pmv)
queue('exp-unaligned', 'exposures', '-o', path('exp-out-unaligned'), *pmv)
# the reference: the moved frames put back with the true transforms, by
# the same warp, then fused (so that only the found transforms differ)
for k in range(5):
    t = invert(motion[k])
    queue('exp-truewarp-%d' % k, 'warp', '--transform=%.12g,%.12g,%.12g,%.12g' % t,
          '-o', path('exp-true-%02d' % k), pmv[k])
queue('exp-truefused', 'exposures', '-o', path('exp-out-true'),
      *[path('exp-true-%02d' % k) for k in range(5)])
fmotion = [(1.0, 0.0, 0.5 * (k - 4) + 0.13 * k, -0.3 * (k - 4)) for k in range(n)]
fmotion[4] = (1.0, 0.0, 0.0, 0.0)
fmoved = synth.focus_stack(sharp, depth, n, 1.2, motion=fmotion)
pfm_ = save_stack('focusmoved', fmoved)
queue('focus-transforms', 'align', '--model=translation', *pfm_)
queue('focus-aligned', 'focus', '--align=translation', '-o', path('focus-out-aligned'), *pfm_)

log = run_all()
if 'JOB FAILED' in log:
    print(log[-3000:])
    check(False, 'every fusion-cli run succeeded')

# ------------------------------------------------------------ evaluate

res = results.setdefault('exposures', {})
for i, (label, paths, frames, m, sig, lev, opts) in enumerate(cases):
    got = pfm.read(path('exp-out-%d' % i))
    ref = mertens.exposure_fusion(frames, m, sig, lev or None)
    d = float(np.abs(got - ref).max())
    res[label] = {'max_abs_diff': d, 'psnr_db': psnr(got, np.asarray(ref)),
                  'pixels_off_1e-4': float(np.mean(np.abs(got - ref).max(axis=-1) > 1e-4)
                                           if got.ndim == 3 else np.mean(np.abs(got - ref) > 1e-4))}
    if '16 bits' in label:
        # 16-bit ramps have contrasts between Mertens' 1e-12 and float
        # resolution: there the reference with the same rule is the measure
        sref = mertens.exposure_fusion(frames, m, sig, lev or None, snap=True)
        ds = float(np.abs(got - sref).max())
        res[label]['max_abs_diff_same_rule'] = ds
        check(ds < 5e-4,
              'exposure fusion = numpy port of Mertens\' code with the rule for contrast below '
              '1e-6: %s: largest difference %.2g; the literal port differs by up to %.2g on '
              '%.2f %% of the pixels (%.1f dB)' % (label, ds, d,
                                                   100 * res[label]['pixels_off_1e-4'],
                                                   psnr(got, ref)))
        continue
    check(d < 2e-4, 'exposure fusion = numpy port of Mertens\' code: %s: largest difference '
          '%.2g (%.1f dB)' % (label, d, psnr(got, ref)))
got = pfm.read(path('exp-out-clip'))
ref = np.clip(mertens.exposure_fusion(fr8), 0, 1)
check(np.abs(got - ref).max() < 2e-4 and got.min() >= 0 and got.max() <= 1,
      'exposure fusion clipped to 0..1 = the reference clipped')

try:
    sys.path.insert(0, os.environ.get('FUSION_CV_SITE', ''))
    import cv2
    merge = cv2.createMergeMertens(1.0, 1.0, 1.0)
    bgr = [np.round(f[..., ::-1] * 255).astype(np.uint8) for f in fr8]
    cvres = merge.process(bgr)[..., ::-1]
    got = pfm.read(path('exp-out-cv'))
    inner = psnr(got, cvres, 16)
    full = psnr(got, cvres)
    res['opencv_same_levels_psnr_db_inner'] = psnr(pfm.read(path('exp-out-0')), cvres, 16)
    res['opencv'] = {'version': cv2.__version__, 'psnr_db_inner': inner, 'psnr_db_full': full,
                     'max_abs_diff_inner': float(np.abs(got - cvres)[16:-16, 16:-16].max())}
    check(inner > 35, 'exposure fusion against OpenCV %s MergeMertens (independent, '
          'Apache-2.0), which builds one pyramid level more than Mertens\' code: %.1f dB '
          'inside a 16 px border, %.1f dB overall with that many levels (%.1f dB with the '
          'paper\'s); it differs in edge handling and in the gray conversion'
          % (cv2.__version__, inner, full, res['opencv_same_levels_psnr_db_inner']))
except ImportError:
    print('XC SKIP OpenCV not available (tests/crosscheck.sh installs it into tests/output/venv)')


def depth_stats(d, truth, conf_mask):
    sel = conf_mask & (d >= 0)
    r = float(np.corrcoef(d[sel], truth[sel])[0, 1])
    mae = float(np.mean(np.abs(d[sel] - truth[sel])))
    return r, mae


res = results.setdefault('focus', {})
best_slice = max(psnr(s, sharp, 8) for s in stack)
best_noisy = max(psnr(s, sharp, 8) for s in noisy)
res['best_single_slice_psnr_db'] = best_slice
inner = np.zeros(depth.shape, bool)
inner[8:-8, 8:-8] = True
for m in methods:
    got = pfm.read(path('focus-out-' + m))
    p = psnr(got, sharp, 8)
    d = pfm.read(path('focus-depth-' + m))
    r, mae = depth_stats(d, depth, inner)
    gn = pfm.read(path('focusn-out-' + m))
    pn_ = psnr(gn, sharp, 8)
    res[m] = {'psnr_db': p, 'depth_r': r, 'depth_mae_slices': mae, 'noisy_psnr_db': pn_}
    check(p > best_slice + 8, 'focus stack (%s), 8 RGB slices: %.1f dB against the sharp '
          'image (best single slice %.1f dB)' % (m, p, best_slice))
    check(pn_ > best_noisy + 3, 'focus stack (%s), noisy 8-bit slices: %.1f dB (best single '
          'slice %.1f dB)' % (m, pn_, best_noisy))
    if m == 'pyramid':
        check(r > 0.97 and mae < 0.35, 'depth map: correlation %.4f with the true depth, mean '
              'error %.3f slices' % (r, mae))
got = pfm.read(path('focus-out-var'))
d = pfm.read(path('focus-depth-var'))
r, mae = depth_stats(d, depth, inner)
res['variance'] = {'psnr_db': psnr(got, sharp, 8), 'depth_r': r, 'depth_mae_slices': mae}
check(psnr(got, sharp, 8) > best_slice + 5, 'focus stack with the local variance measure: '
      '%.1f dB, depth correlation %.4f' % (psnr(got, sharp, 8), r))

res = results.setdefault('microscopy', {})
mid = len(mslices) // 2
trans = transforms_from_log(log, 'micro-transforms')
errs = [corner_error(trans[k], invert(mmotion[k]), 256, 256) for k in range(len(mslices))]
res['transform_corner_error_px'] = max(errs)
check(max(errs) < 0.75, 'microscopy stack, 12 noisy 16-bit slices, magnification 0.2 %% per '
      'slice and drift, scale model: transforms found to %.3f pixels at the corners (largest, '
      'at the ends of the stack; defocus makes blobs look larger, which the scale model '
      'partly takes for magnification)' % max(errs))
# the truth in the frame of the middle slice (which is not moved)
for tag in ('none', 'scale'):
    got = pfm.read(path('micro-out-' + tag))
    d = pfm.read(path('micro-depth-' + tag))
    pk = psnr(got / 0.4, msharp / 0.4, 24)
    cells = np.pad(np.ones((208, 208), bool), 24) & (msharp > 0.05)
    r, _ = depth_stats(d, mdepth, cells)
    res[tag] = {'psnr_db_range_0.4': pk, 'depth_r_on_cells': r}
single = max(psnr(s / 0.4, msharp / 0.4, 24) for s in mslices)
check(res['scale']['psnr_db_range_0.4'] > res['none']['psnr_db_range_0.4'] + 2 and
      res['scale']['psnr_db_range_0.4'] > single + 3,
      'microscopy: aligned %.1f dB, not aligned %.1f dB, best slice %.1f dB (range 0..0.4); '
      'depth correlation on the cells %.3f' % (res['scale']['psnr_db_range_0.4'],
                                               res['none']['psnr_db_range_0.4'], single,
                                               res['scale']['depth_r_on_cells']))

res = results.setdefault('alignment', {})
trans = transforms_from_log(log, 'exp-transforms')
errs = [corner_error(trans[k], invert(motion[k]), 480, 320) for k in range(5)]
res['exposures_corner_error_px'] = errs
check(max(errs) < 0.3, 'bracket of 5 (EV -4 to +4) moved by up to 4.7 px and 0.34 degrees: '
      'found to %s pixels at the corners' % ', '.join('%.3f' % e for e in errs))
a = pfm.read(path('exp-out-aligned'))
u = pfm.read(path('exp-out-unaligned'))
ref = pfm.read(path('exp-out-true'))
pa, pu = psnr(a, ref, 8), psnr(u, ref, 8)
res['exposures_fused_psnr_db'] = {'aligned': pa, 'unaligned': pu}
check(pa > pu + 15 and pa > 40, 'fusion of the moved bracket after alignment: %.1f dB against '
      'the frames put back with the true transforms (without alignment %.1f dB)' % (pa, pu))
trans = transforms_from_log(log, 'focus-transforms')
errs = [corner_error(trans[k], invert(fmotion[k]), w, h) for k in range(n)]
res['focus_corner_error_px'] = errs
check(max(errs) < 0.2, 'focus stack of 8 drifting by up to 2.5 px: shifts found to %.3f '
      'pixels (largest)' % max(errs))
got = pfm.read(path('focus-out-aligned'))
p = psnr(got, sharp, 16)
res['focus_aligned_psnr_db'] = p
check(p > best_slice + 6, 'focus stack of the drifting slices after alignment: %.1f dB '
      'against the sharp image' % p)

json.dump(results, open(os.path.join(here, 'output', 'crosscheck.json'), 'w'), indent=1)
print('XC failures: %d' % failures)
sys.exit(1 if failures else 0)
