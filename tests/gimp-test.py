# Runs inside GIMP 3 without a window (tests/gimp-test.sh starts it): makes
# images of every precision from the PFM files of tests/gimp-inputs.py,
# runs Image > Merge Exposures, Focus Stack and Align Layers by Content as
# a script would, and writes what they made into FT_OUT as PFM files for
# tests/gimp-test.sh to compare with fusion-cli. Prints "FT PASS ..." or
# "FT FAIL ..." for what it can check itself (layers, names, order,
# alpha, errors, menus).
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
import array
import os
import sys

import gi
gi.require_version('Gimp', '3.0')
gi.require_version('Gegl', '0.4')
from gi.repository import Gegl, Gimp  # noqa: E402

inp = os.environ['FT_IN']
out = os.environ['FT_OUT']
only = os.environ.get('FT_ONLY', '')
pdb = Gimp.get_pdb()
failures = 0

P = Gimp.Precision


def check(ok, what):
    global failures
    print('FT %s %s' % ('PASS' if ok else 'FAIL', what), flush=True)
    if not ok:
        failures += 1


def read_pfm(path):
    with open(path, 'rb') as f:
        kind = f.readline().strip()
        w, h = map(int, f.readline().split())
        f.readline()
        c = 3 if kind == b'PF' else 1
        data = f.read(w * h * c * 4)
    row = w * c * 4
    top = b''.join(data[(h - 1 - y) * row:(h - y) * row] for y in range(h))
    return w, h, c, top


def write_pfm(path, w, h, c, top):
    row = w * c * 4
    with open(path, 'wb') as f:
        f.write(b'%s\n%d %d\n-1.0\n' % (b'PF' if c == 3 else b'Pf', w, h))
        f.write(b''.join(top[(h - 1 - y) * row:(h - y) * row] for y in range(h)))


def fmt_of(c, linear, alpha=False):
    if c == 3:
        return ('RGBA float' if alpha else 'RGB float') if linear else \
            ("R'G'B'A float" if alpha else "R'G'B' float")
    return ('YA float' if alpha else 'Y float') if linear else ("Y'A float" if alpha else "Y' float")


def make_image(name, count, precision, linear=False, pick=None, tag=None):
    """an image of the PFM frames NAME-00 ..., bottom first; with a tag,
    the layers are also written as GIMP reads them back (in-TAG-00 ...), for
    fusion-cli to run on exactly the same values"""
    files = [os.path.join(inp, '%s-%02d.pfm' % (name, i)) for i in range(count)]
    if pick is not None:
        files = [files[i] for i in pick]
    w, h, c, _ = read_pfm(files[0])
    base = Gimp.ImageBaseType.RGB if c == 3 else Gimp.ImageBaseType.GRAY
    image = Gimp.Image.new_with_precision(w, h, base, precision)
    for i, fn in enumerate(files):
        _, _, _, data = read_pfm(fn)
        layer = Gimp.Layer.new(image, 'frame %d' % i, w, h,
                               Gimp.ImageType.RGB_IMAGE if c == 3 else Gimp.ImageType.GRAY_IMAGE,
                               100.0, Gimp.LayerMode.NORMAL)
        image.insert_layer(layer, None, 0)
        buf = layer.get_buffer()
        buf.set(Gegl.Rectangle.new(0, 0, w, h), fmt_of(c, linear), data)
        buf.flush()
        layer.update(0, 0, w, h)
        if tag:
            dump(layer, 'in-%s-%02d' % (tag, i), linear)
    if tag:
        tags[image.get_id()] = (tag, len(files))
    return image


tags = {}


def take_plugin_inputs():
    """the layers as the plug-in read them (FUSION_DUMP_INPUTS), in place of
    this script's own reading, for the tagged images"""
    d = os.environ.get('FUSION_DUMP_INPUTS')
    for image_id, (tag, n) in tags.items():
        for k in range(n):
            src = os.path.join(d, 'image-%d-layer-%d.pfm' % (image_id, k))
            if d and os.path.exists(src):
                os.replace(src, os.path.join(out, 'in-%s-%02d.pfm' % (tag, k)))


def dump(drawable, name, linear=False):
    """the drawable's colors (and its alpha, as NAME-alpha.pfm, if it has
    one), read as the plug-in reads layers: always with alpha"""
    buf = drawable.get_buffer()
    r = buf.get_extent()
    c = 1 if drawable.is_gray() else 3
    raw = buf.get(r, 1.0, fmt_of(c, linear, True), Gegl.AbyssPolicy.NONE)
    a = array.array('f')
    a.frombytes(bytes(raw))
    n, step = r.width * r.height, c + 1
    cols = array.array('f', [0.0]) * (n * c)
    for ch in range(c):
        cols[ch::c] = a[ch::step]
    if drawable.has_alpha():
        write_pfm(os.path.join(out, name + '-alpha.pfm'), r.width, r.height, 1,
                  a[c::step].tobytes())
    write_pfm(os.path.join(out, name + '.pfm'), r.width, r.height, c, cols.tobytes())


def run(proc_name, image, drawables=None, **props):
    proc = pdb.lookup_procedure(proc_name)
    config = proc.create_config()
    config.set_property('run-mode', Gimp.RunMode.NONINTERACTIVE)
    config.set_property('image', image)
    config.set_core_object_array('drawables', drawables or image.get_selected_drawables())
    for k, v in props.items():
        config.set_property(k.replace('_', '-'), v)
    result = proc.run(config)
    return result


def status(result):
    return result.index(0)


def case(name):
    return not only or only in name


EXP = 'plug-in-fusion-merge-exposures'
FOCUS = 'plug-in-fusion-focus-stack'
ALIGN = 'plug-in-fusion-align-layers'

# ---------------------------------------------------------------- menus
if case('menus'):
    for proc_name, label in ((EXP, 'Merge E_xposures...'), (FOCUS, '_Focus Stack...'),
                             (ALIGN, 'Align Layers by Co_ntent...')):
        proc = pdb.lookup_procedure(proc_name)
        check(proc is not None and proc.get_menu_label() == label and
              '<Image>/Image/[Arrange]' in proc.get_menu_paths(),
              '%s is in Image > [Arrange] as "%s"' % (proc_name, label))

# ------------------------------------------------------------ exposures
if case('exp-u8'):
    image = make_image('exp', 5, P.U8_NON_LINEAR, tag='u8')
    before = len(image.get_layers())
    r = run(EXP, image)
    check(status(r) == Gimp.PDBStatusType.SUCCESS, 'Merge Exposures on an 8-bit image: success')
    layers = image.get_layers()
    res = r.index(1)
    check(len(layers) == before + 1 and layers[0] == res and res.get_name() == 'Merged exposures'
          and not res.has_alpha(), 'one new opaque layer "Merged exposures" on top')
    check(image.get_selected_layers() == [res], 'the new layer is the selected one')
    dump(res, 'exp-u8')
    image.delete()

if case('exp-float'):
    image = make_image('exp', 5, P.FLOAT_NON_LINEAR)
    dump(run(EXP, image).index(1), 'exp-float')
    image.delete()
    image = make_image('exp', 5, P.FLOAT_NON_LINEAR)
    dump(run(EXP, image, clip=False).index(1), 'exp-float-noclip')
    image.delete()
    image = make_image('exp', 5, P.FLOAT_NON_LINEAR)
    dump(run(EXP, image, contrast=2.0, saturation=0.5, exposure=1.5, exposure_width=0.3,
             levels=5).index(1), 'exp-weights')
    image.delete()

if case('exp-u16'):
    image = make_image('exp16', 3, P.U16_NON_LINEAR, tag='u16')
    dump(run(EXP, image).index(1), 'exp-u16')
    image.delete()

if case('exp-linear'):
    image = make_image('exp', 5, P.FLOAT_NON_LINEAR)
    r = run(EXP, image, space='linear')
    check(status(r) == Gimp.PDBStatusType.SUCCESS, 'Merge Exposures in linear light: success')
    dump(r.index(1), 'exp-linear')
    image.delete()

if case('exp-selected'):
    image = make_image('exp', 5, P.U8_NON_LINEAR)
    lay = image.get_layers()          # top first: frame 4 .. frame 0
    # frames 4, 1 and 2, selected in that odd order
    image.set_selected_layers([lay[0], lay[3], lay[2]])
    r = run(EXP, image)
    res = r.index(1)
    check(image.get_layers()[0] == res, 'with layers selected, the result goes above the '
          'topmost selected one')
    dump(res, 'exp-sel')
    image.delete()

if case('exp-group'):
    image = make_image('exp', 5, P.U8_NON_LINEAR)
    group = Gimp.GroupLayer.new(image, 'brackets')
    image.insert_layer(group, None, 0)
    for layer in list(image.get_layers())[1:][::-1]:
        image.reorder_item(layer, group, 0)
    children = group.get_children()
    image.set_selected_layers(children)
    r = run(EXP, image)
    res = r.index(1)
    check(status(r) == Gimp.PDBStatusType.SUCCESS and res.get_parent() == group and
          group.get_children()[0] == res, 'the layers inside a group: the result goes into '
          'the group, on top')
    dump(res, 'exp-group')
    image.delete()

if case('exp-errors'):
    image = make_image('exp', 1, P.U8_NON_LINEAR)
    r = run(EXP, image)
    check(status(r) == Gimp.PDBStatusType.EXECUTION_ERROR and len(image.get_layers()) == 1,
          'one layer: an error, and the image is left as it was')
    msg = str(r.index(1)) if r.length() > 1 else ''
    check('two or more' in msg, 'the error says what is needed: "%s"' % msg)
    image.delete()
    image = make_image('exp', 5, P.U8_NON_LINEAR)
    lay = image.get_layers()
    image.set_selected_layers([lay[0]])
    r = run(EXP, image, layers='selected')
    check(status(r) == Gimp.PDBStatusType.EXECUTION_ERROR,
          'Layers: selected with one layer selected is an error')
    r = run(EXP, image, layers='auto')
    check(status(r) == Gimp.PDBStatusType.SUCCESS and len(image.get_layers()) == 6,
          'Layers: auto with one layer selected takes all visible layers')
    image.delete()

if case('exp-alpha'):
    image = make_image('exp', 5, P.FLOAT_NON_LINEAR)
    lay = image.get_layers()
    # every layer shifted right by 12 pixels: nothing covers the left strip
    for layer in lay:
        layer.set_offsets(12, 0)
    r = run(EXP, image)
    res = r.index(1)
    check(res.has_alpha(), 'layers that leave part of the canvas empty: the result has alpha')
    dump(res, 'exp-offset')
    image.delete()

if case('exp-mask'):
    image = make_image('exp', 5, P.FLOAT_NON_LINEAR)
    top = image.get_layers()[0]      # frame 4, the brightest
    mask = top.create_mask(Gimp.AddMaskType.WHITE)
    top.add_mask(mask)
    w, h = image.get_width(), image.get_height()
    buf = mask.get_buffer()
    zeros = array.array('f', [0.0]) * (w // 2 * h)
    buf.set(Gegl.Rectangle.new(0, 0, w // 2, h), 'Y float', zeros.tobytes())
    buf.flush()
    dump(run(EXP, image, levels=3).index(1), 'exp-mask')
    image.delete()

if case('exp-align'):
    image = make_image('expmoved', 5, P.U8_NON_LINEAR, tag='moved-u8')
    dump(run(EXP, image, align='translation').index(1), 'exp-aligned')
    image.delete()
    image = make_image('expmoved', 5, P.U8_NON_LINEAR)
    w, h = image.get_width(), image.get_height()
    r = run(EXP, image, align='translation', crop=True)
    check(status(r) == Gimp.PDBStatusType.SUCCESS and image.get_width() < w and
          image.get_height() < h, 'Crop to the common area: %d x %d from %d x %d'
          % (image.get_width(), image.get_height(), w, h))
    with open(os.path.join(out, 'crop.txt'), 'w') as f:
        f.write('%d %d\n' % (image.get_width(), image.get_height()))
    image.delete()

if case('align-layers'):
    image = make_image('expmoved', 5, P.FLOAT_NON_LINEAR)
    r = run(ALIGN, image, model='translation')
    check(status(r) == Gimp.PDBStatusType.SUCCESS and len(image.get_layers()) == 5,
          'Align Layers by Content: success, no layer added')
    same = True
    for i, layer in enumerate(image.get_layers()[::-1]):
        _, ox, oy = layer.get_offsets()
        same &= (layer.get_width(), layer.get_height(), ox, oy) == \
            (image.get_width(), image.get_height(), 0, 0)
        dump(layer, 'aligned-%02d' % i)
    check(same, 'the aligned layers keep their size and place')
    image.delete()

# ---------------------------------------------------------------- focus
if case('micro'):
    image = make_image('micro', 9, P.U16_LINEAR, linear=True, tag='micro')
    r = run(FOCUS, image, depth_map=True)
    check(status(r) == Gimp.PDBStatusType.SUCCESS, 'Focus Stack on a 16-bit linear gray image: '
          'success')
    res, dl = r.index(1), r.index(2)
    lay = image.get_layers()
    check(res is not None and dl is not None and lay[0] == res and lay[1] == dl and
          res.get_name() == 'Focus stack' and dl.get_name() == 'Focus stack depth' and
          not dl.get_visible() and res.is_gray() and dl.is_gray(),
          'the stacked layer on top, the depth map under it, hidden, both gray')
    dump(res, 'micro-pyramid', linear=True)
    dump(dl, 'micro-depth', linear=True)
    image.delete()
    image = make_image('micro', 9, P.U16_LINEAR, linear=True)
    dump(run(FOCUS, image, method='pixels').index(1), 'micro-pixels', linear=True)
    image.delete()
    image = make_image('micro', 9, P.U16_LINEAR, linear=True)
    dump(run(FOCUS, image, method='soft', power=3.0).index(1), 'micro-soft', linear=True)
    image.delete()

if case('focus-rgb'):
    image = make_image('focus', 6, P.U8_NON_LINEAR, tag='focus-u8')
    dump(run(FOCUS, image, method='coefficients').index(1), 'focus-coeff')
    image.delete()
    image = make_image('focus', 6, P.U8_NON_LINEAR)
    r = run(FOCUS, image, depth_map=True)
    check(status(r) == Gimp.PDBStatusType.SUCCESS and not r.index(2).is_gray(),
          'Focus Stack on an 8-bit RGB image with a depth map: the depth map is an RGB layer '
          'of gray values')
    dump(r.index(1), 'focus-default')
    image.delete()

# ---------------------------------------------------------- real images
real = os.environ.get('FT_IMAGES', '')
if case('real') and real and os.path.isdir(os.path.join(real, 'mars_mahli_sol5003_grains')):
    from gi.repository import Gio
    d = os.path.join(real, 'mars_mahli_sol5003_grains')
    files = sorted(f for f in os.listdir(d) if f[:2].isdigit())
    image = Gimp.file_load(Gimp.RunMode.NONINTERACTIVE, Gio.File.new_for_path(os.path.join(d, files[0])))
    for f in files[1:]:
        layer = Gimp.file_load_layer(Gimp.RunMode.NONINTERACTIVE, image, Gio.File.new_for_path(os.path.join(d, f)))
        image.insert_layer(layer, None, 0)
    for align in ('none', 'scale'):
        r = run(FOCUS, image, align=align, depth_map=True, crop=(align == 'scale'))
        check(status(r) == Gimp.PDBStatusType.SUCCESS and r.index(2) is not None,
              'Focus Stack on the MAHLI stack (8 JPEG layers), alignment %s, depth map%s: success'
              % (align, ', cropped' if align == 'scale' else ''))
        if status(r) == Gimp.PDBStatusType.SUCCESS:
            image.remove_layer(r.index(1))
            image.remove_layer(r.index(2))
    image.delete()

take_plugin_inputs()
print('FT failures: %d' % failures, flush=True)
quit_proc = pdb.lookup_procedure('gimp-quit')
qc = quit_proc.create_config()
qc.set_property('force', True)
quit_proc.run(qc)
