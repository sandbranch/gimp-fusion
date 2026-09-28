#!/bin/sh
# Times and peak memory for big stacks: 5 exposures of 24 MP (6000 x 4000,
# RGB) and 30 focus slices of 4 MP (2048 x 2048, gray, as microscopes
# give), with and without alignment:
#   - the core alone (fusion-cli --synthetic: made up while read, no files)
#   - the plug-in in GIMP (tests/bench-gimp.py), on 16-bit layers made with
#     GEGL filters; the plug-in reports its own peak memory (FUSION_STATS)
# Run tests/gimp-test.sh first (it installs the plug-in into the test
# profile). GIMP runs isolated from your folders, as in the other tests.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
out=$here/output
# (isolate.sh needs $src)
# shellcheck disable=SC2034
src=$top
GIMP_RUN_HOME=${GIMP_RUN_HOME:-$out/gimp-home}
export GIMP_RUN_HOME
# shellcheck source=SCRIPTDIR/isolate.sh
. "$here/isolate.sh"
snapshot_take "$out/snapshot-bench-before.txt"
gimp_build=${GIMP_BUILD:-$devtools/gimp-build.sh}
status=0

echo "== the core (fusion-cli), $(nproc) threads"
"$gimp_build" "$top" 'c=tests/output/build/fusion-cli
  for a in "exposures --synthetic=6000,4000,5,3" \
           "exposures --synthetic=6000,4000,5,3 --align=similarity" \
           "focus --synthetic=2048,2048,30,1 --depth=/dev/null" \
           "focus --synthetic=2048,2048,30,1 --align=scale" \
           "focus --synthetic=2048,2048,30,1 --method=coeff" \
           "focus --synthetic=6000,4000,5,3"; do
    printf "%-58s " "$a"; $c $a --stats -o /dev/null 2>&1 | sed "s/^stats: //"
  done' || status=1

echo "== the plug-in in GIMP"
[ -x "$out/profile/plug-ins/gimp-fusion/gimp-fusion" ] ||
  { echo "run tests/gimp-test.sh first"; exit 1; }
gimp_run --timeout=1800 --flatpak --filesystem="$top" --env=GIMP3_DIRECTORY="$out/profile" \
  --env=FUSION_STATS=1 --env=BENCH_ONLY="${BENCH_ONLY:-}" -- \
  gimp-console-3.2 --new-instance --no-interface --no-data --no-fonts \
  --batch-interpreter python-fu-eval -b "exec(open('$here/bench-gimp.py').read())" \
  >"$out/bench-gimp.log" 2>&1
grep -E '^BENCH|FUSION_STATS|Traceback|Error' "$out/bench-gimp.log" | sed 's/^.*FUSION_STATS/FUSION_STATS/'
grep -q 'BENCH plug-in' "$out/bench-gimp.log" || status=1
snapshot_check "$out/snapshot-bench-before.txt" "" || status=1
exit $status
