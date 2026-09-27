# PFM (portable float map) files, as tools/fusion-cli.c reads and writes
# them: gray (Pf) or RGB (PF), little endian, bottom row first.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
import numpy as np


def write(path, a):
    a = np.asarray(a, np.float32)
    h, w = a.shape[:2]
    c = 1 if a.ndim == 2 else a.shape[2]
    assert c in (1, 3)
    with open(path, 'wb') as f:
        f.write(b'%s\n%d %d\n-1.0\n' % (b'PF' if c == 3 else b'Pf', w, h))
        f.write(np.ascontiguousarray(a[::-1]).astype('<f4').tobytes())


def read(path):
    with open(path, 'rb') as f:
        kind = f.readline().strip()
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        c = 3 if kind == b'PF' else 1
        a = np.frombuffer(f.read(w * h * c * 4), '<f4' if scale < 0 else '>f4')
    a = a.reshape(h, w, c)[::-1].astype(np.float32)
    return a[..., 0] if c == 1 else a
