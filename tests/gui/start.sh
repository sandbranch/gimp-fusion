#!/bin/sh
# Starts the Flatpak GIMP on a Broadway display, on the port
# GUI_BROADWAY_PORT (the page is http://127.0.0.1:port/), with
# tests/gui/gui-script.py, in the profile of tests/gimp-test.sh (which
# installs the plug-in there). Isolated from your folders like the other
# tests (tests/isolate.sh). broadwayd stops when GIMP quits. GIMP starts
# as a new instance (--new-instance), so that it never hands its script to
# another GIMP that is running, and loads no fonts (--no-fonts).
#
# GIMP starts when tests/output/gui/page-open exists: GIMP places its
# windows for the size of the Broadway screen, which is that of the page
# once a browser shows it (common.sh does so first).
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
here=$(cd "$(dirname "$0")" && pwd)
tests=$(dirname "$here")
src=$(dirname "$tests")
out=$tests/output/gui
profile=$tests/output/profile
port=${GUI_BROADWAY_PORT:-8088}
display=$((port - 8080))
GIMP_RUN_HOME=$tests/output/gui-home
export GIMP_RUN_HOME
# shellcheck source=SCRIPTDIR/../isolate.sh
. "$tests/isolate.sh"
mkdir -p "$out"
# the dialogs start from their defaults: GIMP keeps the settings of the
# last run in the profile
rm -f "$profile"/plug-in-settings/GimpProcedureConfigRun-plug-in-fusion-*.last
# no Welcome dialog: GIMP shows it when the profile is of an older
# version, or when asked to
if ! grep -q config-version "$profile/gimprc" 2>/dev/null; then
    version=$(LC_ALL=C flatpak info org.gimp.GIMP | sed -n 's/^ *Version: *//p')
    printf '(config-version "%s")\n' "$version" >> "$profile/gimprc"
fi
grep -q show-welcome-dialog "$profile/gimprc" 2>/dev/null ||
  echo '(show-welcome-dialog no)' >> "$profile/gimprc"
# one window at the page's top left corner, the size of the page, with
# the Layers dock on the right (as gegl-adjustments does it); GIMP
# rewrites the file when it quits
size=${GUI_VIEW:-1400,900}
cat > "$profile/sessionrc" <<SESSION
(session-info "toplevel"
    (factory-entry "gimp-single-image-window")
    (position 0 0)
    (size ${size%,*} ${size#*,})
    (open-on-exit)
    (aux-info
        (left-docks-width "0")
        (right-docks-width "250")
        (maximized "yes"))
    (gimp-dock
        (side right)
        (book
            (current-page 0)
            (dockable "gimp-layer-list"
                (tab-style icon)
                (preview-size 32)
                (aux-info
                    (show-button-bar "true"))))))
(hide-docks no)
(single-window-mode yes)
(show-tabs yes)
SESSION
# GUI_ASAN=1: the plug-in of tests/gimp-test.sh --asan, in its profile,
# with the SDK (its sanitizer runtimes); reports in tests/output/sanitizer
devel=
if [ "${GUI_ASAN:-0}" = 1 ]; then
    profile=$tests/output/profile-asan
    mkdir -p "$tests/output/sanitizer"
    devel="--devel --env=ASAN_OPTIONS=log_path=$tests/output/sanitizer/gui-asan:detect_leaks=0:allow_user_segv_handler=0"
fi
# shellcheck disable=SC2086
gimp_run --flatpak $devel --filesystem="$src" \
  --env=GDK_BACKEND=broadway --env=BROADWAY_DISPLAY=:$display \
  --env=GIMP3_DIRECTORY="$profile" --env=GUI_OUT="$out" \
  --env=GUI_IMAGES="$tests/output/images" \
  --env=GUI_MODE="${GUI_MODE:-exposures}" -- sh -c \
  "broadwayd --port $port :$display & bw=\$!; trap 'kill \$bw' EXIT; \
   i=0; while [ ! -f '$out/page-open' ] && [ \$i -lt 300 ]; do sleep 0.2; i=\$((i+1)); done; \
   gimp-3.2 --new-instance --no-splash --no-fonts \
   --batch-interpreter python-fu-eval -b \"exec(open('$here/gui-script.py').read())\""
