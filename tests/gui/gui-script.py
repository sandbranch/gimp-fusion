# Runs inside GIMP on a Broadway display (tests/gui/start.sh). Opens a real
# stack as layers of one image and runs a procedure of the plug-in with its
# dialog, as from the menu:
#
#   GUI_MODE=exposures  the CC0 Ageratum bracket, Merge Exposures
#   GUI_MODE=focus      the MAHLI sol 5003 focus stack, Focus Stack
#   GUI_MODE=align      the same stack, Align Layers by Content
#   GUI_MODE=menu       the bracket, no procedure: the test opens the
#                       Image menu
#
# When the dialog is closed, writes the status, the layers and the image
# size to GUI_OUT/result.txt, waits for GUI_OUT/done (the test takes its
# screenshots meanwhile) and quits GIMP.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
import os
import time

import gi
gi.require_version('Gimp', '3.0')
from gi.repository import Gimp, Gio  # noqa: E402

out = os.environ['GUI_OUT']
images = os.environ['GUI_IMAGES']
mode = os.environ.get('GUI_MODE', 'exposures')
pdb = Gimp.get_pdb()


def write(name, lines):
    with open(os.path.join(out, name + '.tmp'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    os.rename(os.path.join(out, name + '.tmp'), os.path.join(out, name))


def wait(name, seconds=300):
    t = time.time()
    while not os.path.exists(os.path.join(out, name)) and time.time() - t < seconds:
        time.sleep(0.5)


def open_stack(folder):
    d = os.path.join(images, folder)
    files = sorted(f for f in os.listdir(d) if f[:2].isdigit())
    image = Gimp.file_load(Gimp.RunMode.NONINTERACTIVE, Gio.File.new_for_path(os.path.join(d, files[0])))
    image.get_layers()[0].set_name(files[0])
    for f in files[1:]:
        layer = Gimp.file_load_layer(Gimp.RunMode.NONINTERACTIVE, image,
                                     Gio.File.new_for_path(os.path.join(d, f)))
        layer.set_name(f)
        image.insert_layer(layer, None, 0)
    image.clean_all()
    return image


folder = 'gimp_ageratum_bracket' if mode in ('exposures', 'menu') else 'mars_mahli_sol5003_grains'
image = open_stack(folder)
display = Gimp.Display.new(image)
Gimp.displays_flush()
write('ready', ['ready'])

if mode == 'menu':
    wait('check')
    write('result.txt', ['layers %d' % len(image.get_layers())])
else:
    proc = pdb.lookup_procedure({'exposures': 'plug-in-fusion-merge-exposures',
                                 'focus': 'plug-in-fusion-focus-stack',
                                 'align': 'plug-in-fusion-align-layers'}[mode])
    time.sleep(2)
    config = proc.create_config()
    config.set_property('run-mode', Gimp.RunMode.INTERACTIVE)
    config.set_property('image', image)
    config.set_core_object_array('drawables', image.get_selected_drawables())
    result = proc.run(config)
    Gimp.displays_flush()
    # the settings the dialog ran with, as libgimp saved them for the next run
    chosen = '?'
    last = os.path.join(os.environ['GIMP3_DIRECTORY'], 'plug-in-settings',
                        'GimpProcedureConfigRun-%s.last' % proc.get_name())
    key = 'model' if mode == 'align' else 'align'
    if os.path.exists(last):
        for ln in open(last):
            if ln.strip().startswith('(%s ' % key):
                chosen = ln.split(None, 1)[1].strip().strip(')').strip('"')
    layers = image.get_layers()
    write('result.txt', ['status %s' % result.index(0).value_nick,
                         'layers %d' % len(layers),
                         'top %s' % layers[0].get_name(),
                         'size %dx%d' % (image.get_width(), image.get_height()),
                         'align %s' % chosen] +
          ['layer %s visible %d' % (l.get_name(), l.get_visible()) for l in layers])

wait('done')
quit_proc = pdb.lookup_procedure('gimp-quit')
qc = quit_proc.create_config()
qc.set_property('force', True)
quit_proc.run(qc)
