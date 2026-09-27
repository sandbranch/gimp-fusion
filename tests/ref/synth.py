# Synthetic test data with known answers, for tests/crosscheck.py and the
# GIMP tests:
#
#   hdr_scene ()        a float radiance scene with a dynamic range of about
#                       2 x 10^5 (sun, sky, a facade in shadow, a lamp, colour
#                       patches, foliage)
#   exposures ()        that scene as a camera would take it: radiance times
#                       2^EV, clipped, sRGB encoded, quantized to 8 or 16 bits;
#                       optionally each frame moved (shift, rotation, scale)
#   focus_stack ()      a sharp image blurred per pixel by |depth - k|, for
#                       k = 0 .. n-1: a stack with known sharp image and depth
#   microscopy_stack () the same for a 16-bit gray fluorescence-like field
#                       (cells on a dark background, Poisson noise), with a
#                       magnification that grows through the stack and a
#                       drift, as focus stacks on a microscope have
#
# Everything is deterministic (seeded).
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
import numpy as np
from scipy.ndimage import gaussian_filter, map_coordinates


def srgb_encode(v):
    v = np.clip(v, 0.0, 1.0)
    return np.where(v <= 0.0031308, 12.92 * v, 1.055 * np.power(v, 1 / 2.4) - 0.055)


def srgb_decode(v):
    return np.where(v <= 0.04045, v / 12.92, np.power((v + 0.055) / 1.055, 2.4))


def blobs(h, w, n, rmin, rmax, rng, channels=3):
    """a broadband texture: many Gaussian blobs of many sizes"""
    img = np.zeros((h, w, channels))
    ys, xs = np.mgrid[0:h, 0:w]
    for _ in range(n):
        cy, cx = rng.uniform(-10, h + 10), rng.uniform(-10, w + 10)
        r = rmin * (rmax / rmin) ** rng.random()
        a = rng.uniform(-1, 1, channels)
        y0, y1 = int(max(0, cy - 4 * r)), int(min(h, cy + 4 * r + 1))
        x0, x1 = int(max(0, cx - 4 * r)), int(min(w, cx + 4 * r + 1))
        if y0 >= y1 or x0 >= x1:
            continue
        g = np.exp(-((ys[y0:y1, x0:x1] - cy) ** 2 + (xs[y0:y1, x0:x1] - cx) ** 2) / (2 * r * r))
        img[y0:y1, x0:x1] += g[..., None] * a
    return img


def hdr_scene(h=320, w=480, seed=1):
    rng = np.random.default_rng(seed)
    ys, xs = np.mgrid[0:h, 0:w] / np.array([h, w])[:, None, None]
    rad = np.zeros((h, w, 3))
    # sky: bright, bluish, brighter towards the sun at the top right
    sky = (30 + 40 * (1 - ys))[..., None] * np.array([0.7, 0.85, 1.2])
    sun = 2e3 * np.exp(-((xs - 0.8) ** 2 + (ys - 0.12) ** 2) / (2 * 0.015 ** 2))
    rad[:] = sky + sun[..., None]
    # clouds
    cl = gaussian_filter(rng.random((h, w)), 6)
    cl = (cl - cl.mean()) / cl.std()
    rad *= (1 + 0.25 * np.clip(cl, -2, 2))[..., None]
    # the facade in shadow, the left 55 %, below a roof line
    facade = (xs < 0.55) & (ys > 0.25 + 0.1 * xs)
    brick = 0.05 + 0.03 * blobs(h, w, 900, 0.8, 6, rng, 1)[..., 0]
    rad[facade] = (brick[..., None] * np.array([1.0, 0.7, 0.5]))[facade]
    # windows, one with a lamp inside
    for i, (x0, y0) in enumerate(((0.08, 0.45), (0.3, 0.45), (0.08, 0.7), (0.3, 0.7))):
        win = (xs > x0) & (xs < x0 + 0.12) & (ys > y0) & (ys < y0 + 0.15)
        rad[win] = [0.01, 0.012, 0.015] if i != 1 else [8.0, 6.0, 3.0]
    lamp = 400 * np.exp(-((xs - 0.36) ** 2 + (ys - 0.52) ** 2) / (2 * 0.01 ** 2))
    rad += lamp[..., None] * np.array([1.0, 0.8, 0.5])
    # colour patches in the sun on the ground, bottom right
    ground = (xs >= 0.55) & (ys > 0.6)
    rad[ground] = [2.0, 1.8, 1.4]
    cols = [[.8, .1, .1], [.1, .7, .1], [.1, .2, .8], [.9, .8, .1], [.8, .2, .8],
            [.1, .8, .8], [.5, .5, .5], [.95, .95, .95]]
    for i, c in enumerate(cols):
        x0, y0 = 0.6 + (i % 4) * 0.09, 0.65 + (i // 4) * 0.13
        p = (xs > x0) & (xs < x0 + 0.07) & (ys > y0) & (ys < y0 + 0.1)
        rad[p] = 2.5 * np.array(c)
    # foliage: dark and bright green texture between facade and patches
    fol = (xs >= 0.55) & (ys > 0.4) & (ys <= 0.6)
    leaves = np.clip(0.3 + blobs(h, w, 1500, 0.7, 3, rng, 1)[..., 0], 0.02, 3)
    rad[fol] = (leaves[..., None] * np.array([0.25, 0.6, 0.15]))[fol]
    return np.maximum(rad, 1e-4)


def warp_image(img, a=1.0, b=0.0, tx=0.0, ty=0.0, order=3):
    """img sampled at the transform of fz.h: x' - c = [a -b; b a] (x - c) + t"""
    h, w = img.shape[:2]
    cy, cx = (h - 1) / 2, (w - 1) / 2
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float64)
    dx, dy = xs - cx, ys - cy
    sx = cx + a * dx - b * dy + tx
    sy = cy + b * dx + a * dy + ty
    if img.ndim == 2:
        return map_coordinates(img, [sy, sx], order=order, mode='nearest')
    return np.stack([map_coordinates(img[..., c], [sy, sx], order=order, mode='nearest')
                     for c in range(img.shape[2])], axis=-1)


def exposures(rad, evs, bits=8, motion=None, scale=0.25):
    """the frames a camera takes: rad * scale * 2^ev, clipped, sRGB,
    quantized; motion: per frame (a, b, tx, ty), the transform that maps
    the reference frame to the frame (as fz_align finds it)"""
    q = (1 << bits) - 1
    frames = []
    for i, ev in enumerate(evs):
        r = rad
        if motion is not None:
            r = warp_image(np.log(rad), *motion[i])
            r = np.exp(r)
        v = srgb_encode(r * scale * 2.0 ** ev)
        frames.append(np.round(v * q) / q)
    return frames


def _spatial_blur(sharp, sigma, smax=8.0, step=0.25):
    """sharp blurred by a different sigma at every pixel, from blurs at a
    grid of sigmas interpolated linearly"""
    grid = np.arange(0.0, smax + step / 2, step)
    sig = np.clip(sigma, 0, smax)
    j = np.minimum((sig / step).astype(int), len(grid) - 2)
    f = (sig - grid[j]) / step
    out = np.zeros_like(sharp)
    ch = sharp.ndim == 3
    for k, s in enumerate(grid):
        sel0 = j == k
        sel1 = j == k - 1
        if not (sel0.any() or sel1.any()):
            continue
        if s == 0:
            b = sharp
        elif ch:
            b = np.stack([gaussian_filter(sharp[..., c], s, mode='nearest')
                          for c in range(sharp.shape[2])], axis=-1)
        else:
            b = gaussian_filter(sharp, s, mode='nearest')
        wgt = np.where(sel0, 1 - f, 0) + np.where(sel1, f, 0)
        out += (wgt[..., None] if ch else wgt) * b
    return out


def depth_map(h, w, n, seed=3):
    rng = np.random.default_rng(seed)
    ys, xs = np.mgrid[0:h, 0:w] / np.array([h, w])[:, None, None]
    d = 0.15 + 0.7 * xs + 0.1 * ys
    for _ in range(3):
        cx, cy, r, a = rng.uniform(0.2, 0.8), rng.uniform(0.2, 0.8), rng.uniform(0.08, 0.2), \
            rng.uniform(-0.3, 0.3)
        d += a * np.exp(-((xs - cx) ** 2 + (ys - cy) ** 2) / (2 * r * r))
    d = np.clip(d, 0, 1)
    return d * (n - 1)


def photo_texture(h, w, seed=2):
    """an RGB scene with detail at every scale, in about 0.05 .. 0.95"""
    rng = np.random.default_rng(seed)
    t = 0.5 + 0.18 * blobs(h, w, int(h * w / 25), 0.7, 12, rng, 3)
    ys, xs = np.mgrid[0:h, 0:w]
    t += 0.08 * np.sin(xs / 7.0)[..., None] * np.array([1, 0.3, -0.5])
    return np.clip(t, 0.03, 0.97)


def focus_stack(sharp, depth, n, blur_per_slice=1.2, noise=0.0, bits=None, seed=4,
                motion=None):
    """slice k: sharp blurred by blur_per_slice * |depth - k|; motion as
    in exposures (), per slice"""
    rng = np.random.default_rng(seed)
    out = []
    for k in range(n):
        s = _spatial_blur(sharp, blur_per_slice * np.abs(depth - k))
        if motion is not None:
            s = warp_image(s, *motion[k])
        if noise > 0:
            s = s + rng.normal(0, noise, s.shape)
        if bits:
            q = (1 << bits) - 1
            s = np.round(np.clip(s, 0, 1) * q) / q
        out.append(s)
    return out


def microscopy_field(h, w, seed=5):
    """linear intensities in about 0 .. 0.4 (a 16-bit camera's lower range):
    cells with textured nuclei on a dark background"""
    rng = np.random.default_rng(seed)
    ys, xs = np.mgrid[0:h, 0:w]
    img = np.full((h, w), 0.02)
    for _ in range(int(h * w / 2500)):
        cy, cx = rng.uniform(0, h), rng.uniform(0, w)
        ry, rx = rng.uniform(8, 20), rng.uniform(8, 20)
        th = rng.uniform(0, np.pi)
        dy, dx = ys - cy, xs - cx
        u = (dx * np.cos(th) + dy * np.sin(th)) / rx
        v = (-dx * np.sin(th) + dy * np.cos(th)) / ry
        r2 = u * u + v * v
        cell = np.clip(1.2 - r2, 0, 1) ** 0.5
        img += 0.08 * cell + 0.15 * (r2 < 0.35) * np.exp(-r2 * 2)
    tex = gaussian_filter(rng.random((h, w)), 0.8)
    tex = (tex - tex.mean()) / tex.std()
    img *= 1 + 0.35 * tex
    return np.clip(img, 0, None)


def microscopy_stack(h=256, w=256, n=12, seed=6, magnification=0.002,
                     drift=(0.35, -0.25), photons=4000.0, blur_per_slice=1.0):
    """(sharp, depth, slices, motion): 16-bit gray slices of microscopy_field
    with Poisson noise; slice k is magnified by 1 + magnification * (k -
    mid) and drifted by drift * (k - mid) (motion maps the middle slice's
    frame to each slice)"""
    rng = np.random.default_rng(seed)
    sharp = microscopy_field(h, w, seed)
    depth = depth_map(h, w, n, seed + 1)
    mid = n // 2
    motion = [(1 + magnification * (k - mid), 0.0, drift[0] * (k - mid), drift[1] * (k - mid))
              for k in range(n)]
    clean = focus_stack(sharp, depth, n, blur_per_slice, motion=motion)
    slices = []
    for s in clean:
        s = rng.poisson(np.clip(s, 0, None) * photons) / photons
        slices.append(np.round(np.clip(s, 0, 1) * 65535) / 65535)
    return sharp, depth, slices, motion
