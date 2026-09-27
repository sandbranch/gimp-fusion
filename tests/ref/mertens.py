# Exposure fusion (Mertens, Kautz and Van Reeth 2007) in numpy, float64:
# a literal port of Tom Mertens' Matlab reference implementation
# (github.com/Mericam/exposure-fusion: exposure_fusion.m, downsample.m,
# upsample.m, gaussian_pyramid.m, laplacian_pyramid.m,
# reconstruct_laplacian_pyramid.m), used by the tests as the reference
# the C code must match. Matlab's imfilter edge modes map to
# scipy.ndimage: 'replicate' -> 'nearest', 'symmetric' -> 'reflect', the
# default zero padding -> 'constant'.
#
# That code's licence asks for its notice to be kept:
#
#   Copyright (c) 2015, Tom Mertens
#   All rights reserved.
#
#   Redistribution and use in source and binary forms, with or without
#   modification, are permitted provided that the following conditions
#   are met:
#
#   * Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.
#
#   * Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the
#     distribution.
#
#   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Copyright 2026 David (the port)
# SPDX-License-Identifier: GPL-3.0-or-later
import numpy as np
from scipy.ndimage import correlate1d

FILTER = np.array([.0625, .25, .375, .25, .0625])


def imfilter_sep(img, mode):
    """imfilter (I, f, mode) then imfilter (R, f', mode), on axes 1 and 0"""
    r = correlate1d(img, FILTER, axis=1, mode=mode, cval=0.0)
    return correlate1d(r, FILTER, axis=0, mode=mode, cval=0.0)


def downsample(img):
    return imfilter_sep(img, 'reflect')[::2, ::2]


def upsample(img, odd):
    # padarray (I, [1 1 0], 'replicate')
    pad = [(1, 1), (1, 1)] + [(0, 0)] * (img.ndim - 2)
    I = np.pad(img, pad, mode='edge')
    r, c = 2 * I.shape[0], 2 * I.shape[1]
    R = np.zeros((r, c) + img.shape[2:])
    R[0:r:2, 0:c:2] = 4 * I
    R = imfilter_sep(R, 'constant')
    # R (3:r - 2 - odd(1), 3:c - 2 - odd(2), :) in 1-based Matlab
    return R[2:r - 2 - odd[0], 2:c - 2 - odd[1]]


def levels(r, c):
    return int(np.floor(np.log(min(r, c)) / np.log(2)))


def gaussian_pyramid(I, nlev=None):
    nlev = nlev or levels(*I.shape[:2])
    pyr = [I]
    for _ in range(1, nlev):
        I = downsample(I)
        pyr.append(I)
    return pyr


def laplacian_pyramid(I, nlev=None):
    nlev = nlev or levels(*I.shape[:2])
    pyr = []
    J = I
    for _ in range(nlev - 1):
        I = downsample(J)
        odd = (2 * I.shape[0] - J.shape[0], 2 * I.shape[1] - J.shape[1])
        pyr.append(J - upsample(I, odd))
        J = I
    pyr.append(J)
    return pyr


def reconstruct(pyr):
    R = pyr[-1]
    for l in range(len(pyr) - 2, -1, -1):
        odd = (2 * R.shape[0] - pyr[l].shape[0], 2 * R.shape[1] - pyr[l].shape[1])
        R = pyr[l] + upsample(R, odd)
    return R


def rgb2gray(I):
    return I[..., 0] * 0.298936021293775 + I[..., 1] * 0.587043074451121 + \
        I[..., 2] * 0.114020904255103


def contrast(I, snap=False):
    mono = rgb2gray(I) if I.ndim == 3 else I
    h = np.array([1.0, -2.0, 1.0])
    # [0 1 0; 1 -4 1; 0 1 0] with 'replicate' edges = the two second differences
    c = np.abs(correlate1d(mono, h, axis=0, mode='nearest') +
               correlate1d(mono, h, axis=1, mode='nearest'))
    if snap:
        # not in Mertens' code: gimp-fusion's rule (src/core/fusion.c) that
        # contrast below 1e-6 of the value is none
        first = I[..., 0] if I.ndim == 3 else I
        c = np.where(c < 1e-6 * np.maximum(1.0, np.abs(first)), 0.0, c)
    return c


def saturation(I):
    R, G, B = I[..., 0], I[..., 1], I[..., 2]
    mu = (R + G + B) / 3
    return np.sqrt(((R - mu) ** 2 + (G - mu) ** 2 + (B - mu) ** 2) / 3)


def well_exposedness(I, sig=.2):
    if I.ndim == 2:
        return np.exp(-.5 * (I - .5) ** 2 / sig ** 2)
    e = np.ones(I.shape[:2])
    for ch in range(3):
        e = e * np.exp(-.5 * (I[..., ch] - .5) ** 2 / sig ** 2)
    return e


def weights(I, m=(1, 1, 1), sig=.2, snap=False):
    W = np.ones(I.shape[:2])
    if m[0] > 0:
        W = W * contrast(I, snap) ** m[0]
    if m[1] > 0 and I.ndim == 3:
        W = W * saturation(I) ** m[1]
    if m[2] > 0:
        W = W * well_exposedness(I, sig) ** m[2]
    return W


def exposure_fusion(images, m=(1, 1, 1), sig=.2, nlev=None, snap=False):
    """images: a list of H x W x 3 (or H x W) arrays in [0, 1]; snap: with
    gimp-fusion's rule for contrast below float resolution"""
    I = [np.asarray(i, np.float64) for i in images]
    W = np.stack([weights(i, m, sig, snap) for i in I])
    W = W + 1e-12
    W = W / W.sum(axis=0)
    r, c = I[0].shape[:2]
    nlev = nlev or levels(r, c)
    pyr = [np.zeros_like(p) for p in gaussian_pyramid(np.zeros_like(I[0]), nlev)]
    for i in range(len(I)):
        pw = gaussian_pyramid(W[i], nlev)
        pi = laplacian_pyramid(I[i], nlev)
        for l in range(nlev):
            w = pw[l] if I[i].ndim == 2 else pw[l][..., None]
            pyr[l] = pyr[l] + w * pi[l]
    return reconstruct(pyr)
