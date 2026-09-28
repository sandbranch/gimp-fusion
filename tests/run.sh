#!/bin/sh
# All the tests, one after the other:
#
#   tests/check.sh          unit tests of the core, as usual and with ASan and UBSan
#   tests/crosscheck.sh     against Mertens' code (numpy port), OpenCV and
#                           synthetic data with known answers
#   tests/gimp-test.sh      the plug-in in GIMP without a window (also --asan)
#   tests/real.sh           real, freely licensed images (fetched once)
#   tests/gui/gui-test.sh   the dialogs in GIMP on Broadway (skipped without
#                           Chrome and node)
#   tests/bench.sh          times and memory of big stacks (only with
#                           FUSION_BENCH=1)
#
# Everything runs isolated from your folders (tests/isolate.sh); before and
# after, your folders of GIMP and the other apps are listed, and the run
# fails if anything there changed. Exits non-zero if a test failed.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
here=$(cd "$(dirname "$0")" && pwd)
# (isolate.sh needs $src)
# shellcheck disable=SC2034
src=$(dirname "$here")
out=$here/output
mkdir -p "$out"
GIMP_RUN_HOME=${GIMP_RUN_HOME:-$out/gimp-home}
export GIMP_RUN_HOME
# shellcheck source=SCRIPTDIR/isolate.sh
. "$here/isolate.sh"
snapshot_take "$out/snapshot-run-before.txt"
status=0

run () {
    script=$1
    shift
    echo "== tests/$script $*"
    "$here/$script" "$@" || { status=1; echo "FAILED: tests/$script $*"; }
    echo
}
run check.sh
run crosscheck.sh
run gimp-test.sh
run gimp-test.sh --asan
run real.sh
run gui/gui-test.sh
[ "${FUSION_BENCH:-0}" = 1 ] && run bench.sh

snapshot_check "$out/snapshot-run-before.txt" "" || status=1
[ $status = 0 ] && echo "all tests passed" || echo "SOME TESTS FAILED"
exit $status
