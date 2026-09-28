#!/usr/bin/env python3
# The core on real, freely licensed images (tests/images/manifest.json,
# fetched by tests/images/fetch.py):
#
#   gimp_ageratum_bracket   3 JPEG exposures (CC0): fusion against the numpy
#                           port of Mertens' code and OpenCV; the frames'
#                           alignment
#   mars_mahli_*            8-slice focus stacks of the Curiosity rover's
#                           MAHLI camera, which also sent back its own
#                           onboard merge and range map of the same stacks:
#                           our result's sharpness against the best slice
#                           and the onboard merge, and our depth map against
#                           the onboard range map
#   bbbc006_*               9 planes of a 16-bit fluorescence z-stack (CC0):
#                           the result against the plane the dataset calls
#                           best focus
#
# fusion-cli runs through FUSION_RUN (tests/real.sh). Prints "RL PASS ..."
# or "RL FAIL ...", writes the numbers into tests/output/real.json and
# pictures to look at into tests/output/real/.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
import shlex
import subprocess
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter, laplace

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, 'ref'))
import mertens  # noqa: E402
import pfm      # noqa: E402
import synth    # noqa: E402

images = os.path.join(here, 'output', 'images')
out = os.path.join(here, 'output', 'real')
os.makedirs(out, exist_ok=True)
exe = os.environ.get('FUSION_CLI_EXE', os.path.join(here, 'output', 'build', 'fusion-cli'))
failures = 0
results = {}
jobs = []


def check(ok, what):
    global failures
    print('RL %s %s' % ('PASS' if ok else 'FAIL', what), flush=True)
    failures += not ok


def load(path):
    im = Image.open(path)
    if im.mode in ('I;16', 'I;16B', 'I'):
        return np.asarray(im, np.float64) / 65535.0
    if im.mode == 'L':
        return np.asarray(im, np.float64) / 255.0
    return np.asarray(im.convert('RGB'), np.float64) / 255.0


def queue(*args, to=None):
    jobs.append(' '.join(shlex.quote(str(a)) for a in [exe] + list(args)) +
                (' > %s' % shlex.quote(to) if to else ''))


def transforms(name):
    """the rows index, a, b, tx, ty, confidence of an align run"""
    return [list(map(float, ln.split())) for ln in open(os.path.join(out, name + '.align'))
            if len(ln.split()) == 6]


def run_all():
    script = os.path.join(out, 'jobs.sh')
    open(script, 'w').write('set -e\n' + '\n'.join(jobs) + '\n')
    cmd = shlex.split(os.environ.get('FUSION_RUN', '')) + ['sh %s' % shlex.quote(script)] \
        if os.environ.get('FUSION_RUN') else ['sh', script]
    r = subprocess.run(cmd, capture_output=True, text=True)
    open(os.path.join(out, 'jobs.log'), 'w').write(r.stdout + r.stderr)
    return r.returncode == 0, r.stdout


def sharpness(img):
    """mean energy of the Laplacian of the luma, slightly smoothed"""
    y = img if img.ndim == 2 else img @ np.array([0.2126, 0.7152, 0.0722])
    return float(np.mean(laplace(gaussian_filter(y, 0.7)) ** 2))


def save_png(path, a, stretch=False):
    a = np.asarray(a, np.float64)
    if stretch:
        lo, hi = np.percentile(a, 0.5), np.percentile(a, 99.5)
        a = (a - lo) / max(hi - lo, 1e-12)
    Image.fromarray(np.round(np.clip(a, 0, 1) * 255).astype(np.uint8)).save(path)


def frames(name):
    d = os.path.join(images, name)
    fs = sorted(f for f in os.listdir(d) if f[:2].isdigit())
    arr = [load(os.path.join(d, f)) for f in fs]
    paths = []
    for i, a in enumerate(arr):
        p = os.path.join(out, '%s-%02d.pfm' % (name, i))
        pfm.write(p, a)
        paths.append(p)
    return arr, paths


def missing(name):
    if not os.path.isdir(os.path.join(images, name)):
        print('RL SKIP %s: not fetched (tests/images/fetch.py)' % name)
        return True
    return False


sets = {}
if not missing('gimp_ageratum_bracket'):
    a, p = frames('gimp_ageratum_bracket')
    sets['ageratum'] = (a, p)
    queue('exposures', '-o', os.path.join(out, 'ageratum-fused.pfm'), *p)
    queue('exposures', '--no-clip', '-o', os.path.join(out, 'ageratum-noclip.pfm'), *p)
    queue('align', '--model=similarity', *p, to=os.path.join(out, 'ageratum.align'))
for name in ('mars_mahli_sol5020_close', 'mars_mahli_sol5003_grains'):
    if missing(name):
        continue
    a, p = frames(name)
    sets[name] = (a, p)
    for align in ('none', 'scale'):
        queue('focus', '--align=' + align, '--depth=' + os.path.join(out, '%s-%s-depth.pfm' % (name, align)),
              '-o', os.path.join(out, '%s-%s.pfm' % (name, align)), *p)
    queue('align', '--model=scale', '--normalize=0', *p, to=os.path.join(out, name + '.align'))
if not missing('bbbc006_u2os_hoechst_zstack'):
    a, p = frames('bbbc006_u2os_hoechst_zstack')
    sets['bbbc'] = (a, p)
    queue('focus', '--depth=' + os.path.join(out, 'bbbc-depth.pfm'), '-o',
          os.path.join(out, 'bbbc.pfm'), *p)
    queue('focus', '--method=pixels', '-o', os.path.join(out, 'bbbc-pixels.pfm'), *p)

ok, log = run_all()
check(ok, 'every fusion-cli run succeeded')

if 'ageratum' in sets:
    a, _ = sets['ageratum']
    got = pfm.read(os.path.join(out, 'ageratum-noclip.pfm'))
    ref = mertens.exposure_fusion(a)
    d = float(np.abs(got - ref).max())
    res = results.setdefault('ageratum', {'frames': len(a), 'size': list(a[0].shape[:2])})
    res['max_abs_diff_mertens'] = d
    check(d < 2e-4, 'Ageratum bracket (3 JPEG frames): fusion = numpy port of Mertens\' code, '
          'largest difference %.2g' % d)
    try:
        sys.path.insert(0, os.environ.get('FUSION_CV_SITE', ''))
        import cv2
        bgr = [np.round(f[..., ::-1] * 255).astype(np.uint8) for f in a]
        cv = cv2.createMergeMertens(1, 1, 1).process(bgr)[..., ::-1]
        ours_cv = mertens.exposure_fusion(a, nlev=mertens.levels(*a[0].shape[:2]) + 1)
        pv = 10 * np.log10(1 / np.mean((ours_cv - cv)[16:-16, 16:-16] ** 2))
        res['psnr_db_opencv_inner'] = float(pv)
        check(pv > 35, 'Ageratum: the paper\'s method with one level more against OpenCV '
              'MergeMertens: %.1f dB inside a 16 px border' % pv)
    except ImportError:
        pass
    t = transforms('ageratum')
    shift = max(np.hypot(r[3], r[4]) for r in t)
    res['largest_shift_found_px'] = float(shift)
    check(shift < 3, 'Ageratum: the tripod frames are found within %.2f pixels of each other' % shift)
    fused = pfm.read(os.path.join(out, 'ageratum-fused.pfm'))
    save_png(os.path.join(out, 'ageratum-sheet.png'), np.concatenate(list(a) + [fused], axis=1))

for name in ('mars_mahli_sol5020_close', 'mars_mahli_sol5003_grains'):
    if name not in sets:
        continue
    a, _ = sets[name]
    res = results.setdefault(name, {'slices': len(a)})
    onboard = load(os.path.join(images, name, 'onboard_focus_merge_rgb.jpg'))
    rng = load(os.path.join(images, name, 'onboard_range_map_gray.jpg'))
    best = max(sharpness(s) for s in a)
    res['sharpness_best_slice'] = best
    res['sharpness_onboard_merge'] = sharpness(onboard)
    for align in ('none', 'scale'):
        got = pfm.read(os.path.join(out, '%s-%s.pfm' % (name, align)))
        dep = pfm.read(os.path.join(out, '%s-%s-depth.pfm' % (name, align)))
        sh = sharpness(got)
        c = 24
        cc = float(np.corrcoef(got[c:-c, c:-c].ravel(), onboard[c:-c, c:-c].ravel())[0, 1])
        # the onboard range map in its own units: compared by rank correlation
        # where the scene has texture (the confidence of both maps)
        tex = gaussian_filter(np.abs(laplace(gaussian_filter(got @ [0.2126, 0.7152, 0.0722], 1))), 8)
        sel = np.zeros(tex.shape, bool)
        sel[c:-c, c:-c] = True
        sel &= tex > np.percentile(tex[sel], 30)
        rd = np.argsort(np.argsort(dep[sel]))
        rr = np.argsort(np.argsort(rng[sel]))
        rho = float(np.corrcoef(rd, rr)[0, 1])
        res[align] = {'sharpness': sh, 'corr_with_onboard_merge': cc, 'depth_rank_corr_onboard': rho}
    t = transforms(name)
    res['magnification_range'] = [min(x[1] for x in t), max(x[1] for x in t)]
    res['largest_shift_px'] = max(np.hypot(x[3], x[4]) for x in t)
    r = res['scale']
    check(r['sharpness'] > best and r['sharpness'] > 0.95 * res['sharpness_onboard_merge'] and
          r['corr_with_onboard_merge'] > 0.8,
          '%s: our stack (aligned for scale) is sharper than the sharpest slice (%.3g against '
          '%.3g), %.0f %% of the rover\'s onboard merge (%.3g), and correlates %.3f with it'
          % (name, r['sharpness'], best, 100 * r['sharpness'] / res['sharpness_onboard_merge'],
             res['sharpness_onboard_merge'], r['corr_with_onboard_merge']))
    check(abs(r['depth_rank_corr_onboard']) > 0.6,
          '%s: our depth map against the rover\'s range map: rank correlation %.3f (textured '
          'areas; the sign is the maps\' direction)' % (name, r['depth_rank_corr_onboard']))
    # is the magnification found real? neighbours, locally normalized,
    # compared as they are and after the transforms found
    def norm(im):
        y = gaussian_filter(im @ np.array([0.2126, 0.7152, 0.0722]), 1.5)
        d = y - gaussian_filter(y, 6)
        return d / np.sqrt(gaussian_filter(d * d, 6) + 1e-8)

    def ncc(p, q, c=60):
        return float(np.corrcoef(p[c:-c, c:-c].ravel(), q[c:-c, c:-c].ravel())[0, 1])
    warped = [synth.warp_image(a[k], *t[k][1:5], order=1) for k in range(len(a))]
    before = [ncc(norm(a[k]), norm(a[k + 1])) for k in range(len(a) - 1)]
    after = [ncc(norm(warped[k]), norm(warped[k + 1])) for k in range(len(a) - 1)]
    res['neighbour_ncc_as_they_are'] = before
    res['neighbour_ncc_aligned'] = after
    check(min(after) > 0.9 and all(x > y for x, y in zip(after, before)),
          '%s: the magnification MAHLI changes with focus (found %.4f to %.4f): neighbours '
          'match with NCC %.3f to %.3f after alignment, %.3f to %.3f as they are'
          % (name, res['magnification_range'][0], res['magnification_range'][1],
             min(after), max(after), min(before), max(before)))
    got = pfm.read(os.path.join(out, '%s-scale.pfm' % name))
    dep = pfm.read(os.path.join(out, '%s-scale-depth.pfm' % name))
    h, w = got.shape[:2]
    y0, x0 = h // 2 - 150, w // 2 - 200
    crop = lambda im: im[y0:y0 + 300, x0:x0 + 400]  # noqa: E731
    sharpest = max(a, key=sharpness)
    save_png(os.path.join(out, '%s-sheet.png' % name),
             np.concatenate([crop(sharpest), crop(got), crop(onboard)], axis=1))
    save_png(os.path.join(out, '%s-depth.png' % name),
             np.concatenate([dep / (len(a) - 1), rng], axis=1), stretch=False)

if 'bbbc' in sets:
    a, _ = sets['bbbc']
    res = results.setdefault('bbbc006', {'planes': len(a)})
    got = pfm.read(os.path.join(out, 'bbbc.pfm'))
    dep = pfm.read(os.path.join(out, 'bbbc-depth.pfm'))
    pix = pfm.read(os.path.join(out, 'bbbc-pixels.pfm'))
    best_i = 4   # z = 16, which the dataset page calls the best focus
    sh = [sharpness(s) for s in a]
    res['sharpness_per_plane'] = sh
    res['sharpness_stack'] = sharpness(got)
    cells = gaussian_filter(a[best_i], 3) > np.percentile(gaussian_filter(a[best_i], 3), 70)
    res['median_depth_on_nuclei'] = float(np.median(dep[cells]))
    cc = float(np.corrcoef(got.ravel(), a[best_i].ravel())[0, 1])
    res['corr_with_best_plane'] = cc
    check(res['sharpness_stack'] >= 0.95 * max(sh) and cc > 0.95,
          'BBBC006, 9 planes of 16-bit fluorescence: the stack is as sharp as the best plane '
          '(%.3g against %.3g at z = %d) and correlates %.3f with z = 16'
          % (res['sharpness_stack'], max(sh), 8 + 2 * int(np.argmax(sh)), cc))
    check(abs(res['median_depth_on_nuclei'] - best_i) <= 1.5,
          'BBBC006: the depth map on the nuclei is around the plane of best focus (median '
          'index %.2f, z = 16 is index %d)' % (res['median_depth_on_nuclei'], best_i))
    exact = bool(np.isin(pix.ravel()[::97],
                         np.concatenate([s.astype(np.float32).ravel() for s in a])).all())
    check(exact, 'BBBC006, pixels method: every value (a sample of them) is a value of one '
          'of the planes')
    save_png(os.path.join(out, 'bbbc-sheet.png'),
             np.concatenate([a[0], a[best_i], a[-1], got], axis=1), stretch=True)

json.dump(results, open(os.path.join(here, 'output', 'real.json'), 'w'), indent=1)
print('RL failures: %d' % failures)
sys.exit(1 if failures else 0)
