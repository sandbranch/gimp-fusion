#!/bin/sh
# Measures the core against references and ground truth (tests/crosscheck.py):
# exposure fusion against a numpy port of Mertens' Matlab code and against
# OpenCV's MergeMertens, focus stacks and alignment against synthetic data
# with known answers. fusion-cli is built and run inside the Flatpak SDK
# (gimp-devtools/gimp-build.sh), isolated from your folders (tests/output/
# gimp-home); python3 with numpy and scipy runs on the host.
#
# OpenCV comes from PyPI into tests/output/venv, the wheel pinned by its
# SHA-256 (FUSION_NO_OPENCV=1 skips it; without network it is skipped).
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
out=$here/output
mkdir -p "$out"
# (isolate.sh needs $src)
# shellcheck disable=SC2034
src=$top
GIMP_RUN_HOME=${GIMP_RUN_HOME:-$out/gimp-home}
export GIMP_RUN_HOME
# shellcheck source=SCRIPTDIR/isolate.sh
. "$here/isolate.sh"
snapshot_take "$out/snapshot-cross-before.txt"
gimp_build=${GIMP_BUILD:-$devtools/gimp-build.sh}
status=0

if [ ! -f "$out/build/build.ninja" ]; then
    "$gimp_build" "$top" "meson setup tests/output/build" >"$out/setup.log" 2>&1 ||
      { cat "$out/setup.log"; exit 1; }
fi
"$gimp_build" "$top" "ninja -C tests/output/build" >"$out/build.log" 2>&1 ||
  { cat "$out/build.log"; exit 1; }

cv_site=
if [ "${FUSION_NO_OPENCV:-0}" != 1 ]; then
    venv=$out/venv
    if [ ! -x "$venv/bin/python" ]; then
        python3 -m venv --system-site-packages "$venv" >/dev/null 2>&1
        # opencv-python-headless 5.0.0.93, manylinux_2_28 x86_64 (Apache-2.0)
        echo 'opencv-python-headless==5.0.0.93 --hash=sha256:ed709fdf9aa0bd1f2ed8549e71d19449b03a675bb581eb292285f6861953be37' \
          >"$out/opencv-requirements.txt"
        "$venv/bin/pip" install -q --no-deps --require-hashes \
          -r "$out/opencv-requirements.txt" >"$out/venv.log" 2>&1 ||
          echo "XC SKIP OpenCV could not be installed (see $out/venv.log)"
    fi
    cv_site=$("$venv/bin/python" -c 'import cv2, os; print(os.path.dirname(os.path.dirname(cv2.__file__)))' 2>/dev/null)
fi

FUSION_RUN="$gimp_build $top" FUSION_CLI_EXE="$out/build/fusion-cli" FUSION_CV_SITE="$cv_site" \
  python3 "$here/crosscheck.py" || status=1
snapshot_check "$out/snapshot-cross-before.txt" "XC " || status=1
exit $status
