#!/bin/sh
# Tests the plug-in in the Flatpak GIMP without a window: tests/gimp-inputs.py
# writes the inputs and fusion-cli the expected results; tests/gimp-test.py
# runs Merge Exposures, Focus Stack and Align Layers by Content in GIMP on
# images of 8 and 16 bits and floating point, RGB and gray, with selections,
# groups, masks, offsets, alignment and cropping; tests/gimp-compare.py
# compares.
#
#   tests/gimp-test.sh            as usual
#   tests/gimp-test.sh --asan     the plug-in built with AddressSanitizer and
#                                 UBSan (GIMP runs with the SDK: --devel)
#   FT_ONLY=micro tests/gimp-test.sh   only the GIMP cases whose names match
#
# GIMP runs with a throwaway profile in tests/output (the plug-in is
# installed there, nothing in yours) and isolated from your folders
# (tests/isolate.sh): before and after, your folders of GIMP and the other
# apps are listed, and the test fails if anything there changed. GIMP loads
# no fonts and no data (--no-fonts --no-data) and starts as a new instance.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
out=$here/output
g=$out/gimp
mkdir -p "$out"
# (isolate.sh needs $src)
# shellcheck disable=SC2034
src=$top
GIMP_RUN_HOME=${GIMP_RUN_HOME:-$out/gimp-home}
export GIMP_RUN_HOME
# shellcheck source=SCRIPTDIR/isolate.sh
. "$here/isolate.sh"
snapshot_take "$out/snapshot-gimp-before.txt"
gimp_build=${GIMP_BUILD:-$devtools/gimp-build.sh}
in_sdk () { "$gimp_build" "$top" "$*"; }

build=tests/output/build
profile=$out/profile
run_args=
if [ "$1" = --asan ]; then
    build=tests/output/build-asan
    profile=$out/profile-asan
    run_args="--devel --env=ASAN_OPTIONS=log_path=$out/sanitizer/asan:detect_leaks=0 \
      --env=UBSAN_OPTIONS=log_path=$out/sanitizer/ubsan:print_stacktrace=1"
    [ -f "$top/$build/build.ninja" ] ||
      in_sdk meson setup "$build" -Db_sanitize=address,undefined -Db_lundef=false \
        "-Dplugindir=$profile/plug-ins" >"$out/setup-asan.log" 2>&1 ||
      { cat "$out/setup-asan.log"; exit 1; }
fi
[ -f "$top/$build/build.ninja" ] || in_sdk meson setup "$build" >"$out/setup.log" 2>&1 ||
  { cat "$out/setup.log"; exit 1; }
in_sdk meson configure "$build" "-Dplugindir=$profile/plug-ins" >/dev/null
in_sdk ninja -C "$build" install >"$out/build.log" 2>&1 || { cat "$out/build.log"; exit 1; }
rm -rf "$out/sanitizer" "$g"
mkdir -p "$out/sanitizer" "$g/got"
status=0

python3 "$here/gimp-inputs.py" "$top/$build/fusion-cli" >/dev/null || exit 1

# shellcheck disable=SC2086
gimp_run --timeout=1200 --flatpak $run_args --filesystem="$top" \
  --env=GIMP3_DIRECTORY="$profile" --env=FT_IN="$g/in" --env=FT_OUT="$g/got" \
  --env=FT_ONLY="${FT_ONLY:-}" --env=FT_IMAGES="$out/images" -- \
  gimp-console-3.2 --new-instance --no-interface --no-data --no-fonts \
  --batch-interpreter python-fu-eval -b "exec(open('$here/gimp-test.py').read())" \
  >"$g/gimp.log" 2>&1
grep -E '^FT|Traceback|^  File|Error' "$g/gimp.log"
grep -q '^FT failures: 0$' "$g/gimp.log" || status=1
if grep -E 'gimp-fusion.*(WARNING|CRITICAL)|inconsistent state' "$g/gimp.log"; then
    echo "FT FAIL warnings from the plug-in, see $g/gimp.log"
    status=1
fi

if [ -z "${FT_ONLY:-}" ]; then
    # the expected results, from fusion-cli on the layers as GIMP read them
    in_sdk sh tests/output/gimp/expect.sh >"$g/expect.log" 2>&1 ||
      { echo "FT FAIL fusion-cli could not make the expected results"; tail "$g/expect.log"; status=1; }
    # the layers Align Layers by Content moved, aligned once more: nothing left
    # shellcheck disable=SC2086
    in_sdk "$build/fusion-cli align --model=translation $(ls "$g"/got/aligned-*.pfm | tr '\n' ' ')" \
      >"$g/realign.txt" 2>&1
    python3 "$here/gimp-compare.py" || status=1
fi
for report in "$out"/sanitizer/*; do
    [ -e "$report" ] || continue
    echo "FT FAIL sanitizer report $report"
    head -30 "$report"
    status=1
done
snapshot_check "$out/snapshot-gimp-before.txt" "FT " || status=1
[ $status = 0 ] && echo "FT all passed" || echo "FT FAILED (log: $g/gimp.log)"
exit $status
