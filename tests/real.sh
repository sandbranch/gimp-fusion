#!/bin/sh
# The core on real, freely licensed images (tests/real.py): fetches them
# once (tests/images/fetch.py, SHA-1 checked; skipped sets without
# network), runs fusion-cli in the Flatpak SDK, isolated from your folders.
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
snapshot_take "$out/snapshot-real-before.txt"
gimp_build=${GIMP_BUILD:-$devtools/gimp-build.sh}
status=0
python3 "$here/images/fetch.py" || echo "RL SKIP some images could not be fetched"
[ -f "$out/build/build.ninja" ] || "$gimp_build" "$top" meson setup tests/output/build >/dev/null
"$gimp_build" "$top" ninja -C tests/output/build >/dev/null || exit 1
cv_site=$("$out/venv/bin/python" -c 'import cv2, os; print(os.path.dirname(os.path.dirname(cv2.__file__)))' 2>/dev/null)
FUSION_RUN="$gimp_build $top" FUSION_CLI_EXE="$out/build/fusion-cli" FUSION_CV_SITE="$cv_site" \
  python3 "$here/real.py" || status=1
snapshot_check "$out/snapshot-real-before.txt" "RL " || status=1
exit $status
