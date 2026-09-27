#!/bin/sh
# The unit tests of the core (tests/unit), built twice: as usual and with
# AddressSanitizer and UndefinedBehaviorSanitizer, leaks included (the
# core allocates with malloc only, so every leak is ours). Also runs
# fusion-cli once under the sanitizers on a small synthetic stack of each
# kind. Builds and runs inside the Flatpak SDK (gimp-devtools/gimp-build.sh),
# isolated from your folders.
#
#   tests/check.sh           both builds
#   tests/check.sh quick     only the usual build
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
gimp_build=${GIMP_BUILD:-$devtools/gimp-build.sh}
in_sdk () { "$gimp_build" "$top" "$*"; }
status=0

unit () {
    build=$1 env=$2 log=$3
    in_sdk "$env meson test -C $build --print-errorlogs" >"$log" 2>&1 || status=1
    for t in pyramid fusion align; do
        in_sdk "$env $build/test-$t" >"$log.$t" 2>&1 || status=1
        grep -E '^FAIL' "$log.$t"
        echo "$t: $(grep -E '^[0-9]+ passed' "$log.$t" || echo 'did not finish')"
    done
}

echo "== usual build"
[ -f "$out/build/build.ninja" ] || in_sdk meson setup tests/output/build >"$out/setup.log" 2>&1
in_sdk ninja -C tests/output/build >"$out/build.log" 2>&1 || { cat "$out/build.log"; exit 1; }
unit tests/output/build "" "$out/unit.log"
[ "$1" = quick ] && exit $status

echo "== AddressSanitizer and UndefinedBehaviorSanitizer"
[ -f "$out/build-asan/build.ninja" ] ||
  in_sdk meson setup tests/output/build-asan -Db_sanitize=address,undefined -Db_lundef=false \
    >"$out/setup-asan.log" 2>&1
in_sdk ninja -C tests/output/build-asan >"$out/build-asan.log" 2>&1 ||
  { cat "$out/build-asan.log"; exit 1; }
san="ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"
unit tests/output/build-asan "$san" "$out/unit-asan.log"
cli=tests/output/build-asan/fusion-cli
in_sdk "$san $cli exposures --stats --synthetic=203,117,4,3 --align=similarity -o /dev/null && \
  $san $cli focus --stats --synthetic=190,131,5,1 --depth=/dev/null --method=coeff -o /dev/null && \
  $san $cli focus --stats --synthetic=64,33,3,3 --method=soft --align=scale -o /dev/null && \
  $san $cli focus --stats --synthetic=5,3,2,1 --method=pixels -o /dev/null" \
  >"$out/cli-asan.log" 2>&1 || { status=1; echo "FAIL fusion-cli under the sanitizers:"; tail "$out/cli-asan.log"; }
if grep -qE 'ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:' "$out"/unit-asan.log* "$out/cli-asan.log"; then
    echo "FAIL sanitizer reports:"
    grep -h -E -A5 'ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:' "$out"/unit-asan.log* "$out/cli-asan.log" | head -30
    status=1
else
    echo "PASS no memory errors, leaks or undefined behaviour (unit tests and fusion-cli)"
fi
[ $status = 0 ] && echo "all checks passed" || echo "SOME CHECKS FAILED"
exit $status
