#!/bin/sh
# Tests the three dialogs in GIMP's real window, on a Broadway display with
# a headless Chrome, on real freely licensed images (tests/images), and
# leaves screenshots in tests/output/gui/ (with GUI_DOCS=1 also the ones
# for the README in docs/):
#
#   menu       Image menu open: Merge Exposures, Focus Stack and Align
#              Layers by Content next to Merge Visible Layers
#   exposures  the Ageratum bracket (3 layers), Merge Exposures, OK: one
#              new layer "Merged exposures" on top
#   focus      the MAHLI focus stack (8 layers), Focus Stack, Add a depth
#              map and Shift and scale ticked, OK: the stacked layer on top,
#              the hidden depth map under it
#   align      the same stack, Align Layers by Content, OK: 8 layers still
#
# Run tests/gimp-test.sh first (it installs the plug-in into the test
# profile) and tests/images/fetch.py (tests/real.sh does). Needs a headless
# Chrome, node 22 and ../gimp-devtools (or GIMP_PLUGIN_DEVTOOLS) for
# gui/cdp.mjs. GIMP runs isolated from your folders (start.sh); before and
# after, your folders are listed and the test fails if anything there
# changed. Stops only the GIMP and the Chrome it started.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later

# (pass never fails, so "check && pass || fail" is an if-then-else)
# shellcheck disable=SC2015
here=$(cd "$(dirname "$0")" && pwd)
status=0
pass () { echo "GUI PASS $1"; }
fail () { echo "GUI FAIL $1"; status=1; }

# shellcheck source=SCRIPTDIR/common.sh
. "$here/common.sh"
# shellcheck source=SCRIPTDIR/../isolate.sh
. "$tests/isolate.sh"
[ -d "$tests/output/images/mars_mahli_sol5003_grains" ] ||
  { echo "GUI FAIL run tests/images/fetch.py first"; exit 1; }
rm -f "$out"/0*.png
snapshot_take "$out/snapshot-before.txt"

# positions: in the image window (single-window mode, the page's size),
# and from the dialog's top left corner (the Broadway canvas)
IMAGE_MENU=212,49
CANVAS=700,780
EXP_OK=${GUI_EXP_OK:-372,528}
FOCUS_OK=${GUI_FOCUS_OK:-372,675}
FOCUS_DEPTH=${GUI_FOCUS_DEPTH:-50,477}
FOCUS_ALIGN=${GUI_FOCUS_ALIGN:-286,422}
FOCUS_SCALE_ITEM=${GUI_FOCUS_SCALE_ITEM:-200,483}
ALIGN_OK=${GUI_ALIGN_OK:-372,363}

click_in_dialog () {
    pos=$(p "${1%,*}" "${1#*,}")
    $cdp "$view" move:"$(p 100 60)" wait:400 move:"$pos" wait:400 click:"$pos" wait:"${2:-800}" >/dev/null
}

result_is () {
    grep -q "^$1\$" "$out/result.txt" && pass "$2" || fail "$2: $(tr '\n' ' ' < "$out/result.txt")"
}

session () {
    name=$1
    GUI_MODE=$name
    export GUI_MODE
    start_gimp "$name"
    if ! wait_for "$out/ready" 120; then
        fail "$name: GIMP did not get ready (log: $out/gimp-$name.log)"
        stop_all
        return
    fi
    case $name in
    menu)
        sleep 4
        $cdp "$view" move:"$IMAGE_MENU" wait:300 click:"$IMAGE_MENU" wait:1500 \
          shot:"$out/01-image-menu.png" key:Escape wait:300 >/dev/null
        touch "$out/check"
        wait_for "$out/result.txt" 60 && pass "menu: the Image menu opened (see 01-image-menu.png)" ||
          fail "menu: no result";;
    exposures)
        find_dialog
        [ -n "$at" ] || { fail "exposures: the dialog did not open"; stop_all; return; }
        pass "exposures: the dialog opened (at $at)"
        $cdp "$view" wait:800 shot:"$out/02-exposures-dialog.png" >/dev/null
        click_in_dialog "$EXP_OK" 1000
        if ! wait_for "$out/result.txt" 120; then
            fail "exposures: no result after OK (log: $out/gimp-$name.log)"; stop_all; return
        fi
        result_is "status success" "exposures: status success"
        result_is "layers 4" "exposures: 3 layers and the new one"
        result_is "top Merged exposures" "exposures: the new layer \"Merged exposures\" on top"
        $cdp "$view" move:$CANVAS wait:300 key:1 wait:1500 shot:"$out/03-exposures-result.png" >/dev/null;;
    focus)
        find_dialog
        [ -n "$at" ] || { fail "focus: the dialog did not open"; stop_all; return; }
        pass "focus: the dialog opened (at $at)"
        [ "$FOCUS_DEPTH" = 0,0 ] || click_in_dialog "$FOCUS_DEPTH"
        # Align first: the combo box, then "Shift and scale" in its list
        if [ "$FOCUS_ALIGN" != 0,0 ]; then
            click_in_dialog "$FOCUS_ALIGN" 1200
            item=$(p "${FOCUS_SCALE_ITEM%,*}" "${FOCUS_SCALE_ITEM#*,}")
            $cdp "$view" move:"$item" wait:400 click:"$item" wait:1000 >/dev/null
        fi
        $cdp "$view" wait:800 shot:"$out/04-focus-dialog.png" >/dev/null
        [ "$FOCUS_OK" = 0,0 ] && { touch "$out/done"; stop_all; return; }
        click_in_dialog "$FOCUS_OK" 1000
        if ! wait_for "$out/result.txt" 300; then
            fail "focus: no result after OK (log: $out/gimp-$name.log)"; stop_all; return
        fi
        result_is "status success" "focus: status success"
        result_is "layers 10" "focus: 8 layers, the stacked one and the depth map"
        result_is "top Focus stack" "focus: \"Focus stack\" on top"
        result_is "layer Focus stack depth visible 0" "focus: the depth map, hidden"
        grep -q "^align scale$" "$out/result.txt" && pass "focus: aligned with shift and scale (as chosen in the dialog)" ||
          fail "focus: the alignment chosen: $(grep align "$out/result.txt")"
        $cdp "$view" move:$CANVAS wait:300 key:1 wait:1500 shot:"$out/05-focus-result.png" >/dev/null;;
    align)
        find_dialog
        [ -n "$at" ] || { fail "align: the dialog did not open"; stop_all; return; }
        pass "align: the dialog opened (at $at)"
        $cdp "$view" wait:800 shot:"$out/06-align-dialog.png" >/dev/null
        [ "$ALIGN_OK" = 0,0 ] && { touch "$out/done"; stop_all; return; }
        click_in_dialog "$ALIGN_OK" 1000
        if ! wait_for "$out/result.txt" 300; then
            fail "align: no result after OK (log: $out/gimp-$name.log)"; stop_all; return
        fi
        result_is "status success" "align: status success"
        result_is "layers 8" "align: the 8 layers, none added";;
    esac
    touch "$out/done"
    stop_all
}

for s in ${GUI_ONLY:-menu exposures focus align}; do
    session "$s"
done

if [ "${GUI_DOCS:-0}" = 1 ]; then
    mkdir -p "$src/docs"
    python3 - "$out" "$src/docs" <<'PY'
import sys
from PIL import Image
out, docs = sys.argv[1:]
for shot, name, box in (('01-image-menu', 'image-menu', (0, 30, 700, 700)),
                        ('02-exposures-dialog', 'exposures-dialog', None),
                        ('03-exposures-result', 'exposures-result', None),
                        ('04-focus-dialog', 'focus-dialog', None),
                        ('05-focus-result', 'focus-result', None),
                        ('06-align-dialog', 'align-dialog', None)):
    try:
        im = Image.open('%s/%s.png' % (out, shot))
        (im.crop(box) if box else im).save('%s/%s.png' % (docs, name))
    except FileNotFoundError:
        pass
PY
fi
snapshot_check "$out/snapshot-before.txt" "GUI " || status=1
[ $status = 0 ] && echo "GUI all passed (screenshots in $out)" || echo "GUI FAILED (screenshots in $out)"
exit $status
