# Runs inside GIMP (tests/bench.sh): makes the benchmark stacks with GEGL
# filters (plasma, then an exposure change or a blur per layer) and times
# the plug-in on them. The plug-in prints FUSION_STATS lines (time and its
# own peak memory) on stderr, which bench.sh picks up.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
import os
import time

import gi
gi.require_version('Gimp', '3.0')
from gi.repository import Gimp  # noqa: E402

pdb = Gimp.get_pdb()


def gegl(drawable, op, **props):
    f = Gimp.DrawableFilter.new(drawable, op, op)
    c = f.get_config()
    for k, v in props.items():
        c.set_property(k.replace('_', '-'), v)
    f.update()
    drawable.merge_filter(f)


def stack(w, h, n, base, precision, vary):
    image = Gimp.Image.new_with_precision(w, h, base, precision)
    t = Gimp.ImageType.RGB_IMAGE if base == Gimp.ImageBaseType.RGB else Gimp.ImageType.GRAY_IMAGE
    first = Gimp.Layer.new(image, 'base', w, h, t, 100, Gimp.LayerMode.NORMAL)
    image.insert_layer(first, None, 0)
    gegl(first, 'gegl:plasma', turbulence=1.5, seed=7)
    for k in range(n):
        layer = first.copy()
        image.insert_layer(layer, None, 0)
        vary(layer, k)
    image.remove_layer(first)
    return image


def run(proc_name, image, **props):
    proc = pdb.lookup_procedure(proc_name)
    config = proc.create_config()
    config.set_property('run-mode', Gimp.RunMode.NONINTERACTIVE)
    config.set_property('image', image)
    config.set_core_object_array('drawables', image.get_selected_drawables())
    for k, v in props.items():
        config.set_property(k.replace('_', '-'), v)
    t0 = time.time()
    r = proc.run(config)
    print('BENCH %s %s: %s, %.2f s in GIMP' % (proc_name, props, r.index(0).value_nick,
                                              time.time() - t0), flush=True)
    # the new layers go again, so that the next run gets the same stack
    for i in range(1, r.length()):
        layer = r.index(i)
        if layer is not None:
            image.remove_layer(layer)


which = os.environ.get('BENCH_ONLY', '')
if not which or 'exp' in which:
    t0 = time.time()
    image = stack(6000, 4000, 5, Gimp.ImageBaseType.RGB, Gimp.Precision.U16_NON_LINEAR,
                  lambda layer, k: gegl(layer, 'gegl:exposure', exposure=2.0 * (k - 2)))
    print('BENCH made 5 layers of 6000 x 4000, RGB 16-bit, in %.1f s' % (time.time() - t0),
          flush=True)
    run('plug-in-fusion-merge-exposures', image)
    run('plug-in-fusion-merge-exposures', image, align='similarity')
    image.delete()
if not which or 'focus' in which:
    t0 = time.time()
    image = stack(2048, 2048, 30, Gimp.ImageBaseType.GRAY, Gimp.Precision.U16_LINEAR,
                  lambda layer, k: gegl(layer, 'gegl:gaussian-blur',
                                        std_dev_x=0.3 + 0.25 * abs(k - 15),
                                        std_dev_y=0.3 + 0.25 * abs(k - 15)))
    print('BENCH made 30 layers of 2048 x 2048, gray 16-bit linear, in %.1f s'
          % (time.time() - t0), flush=True)
    run('plug-in-fusion-focus-stack', image, depth_map=True)
    run('plug-in-fusion-focus-stack', image, align='scale')
    image.delete()

quit_proc = pdb.lookup_procedure('gimp-quit')
qc = quit_proc.create_config()
qc.set_property('force', True)
quit_proc.run(qc)
