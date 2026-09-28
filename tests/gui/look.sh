#!/bin/sh
# Opens GIMP on a Broadway display in the given mode (plugin or filter,
# see gui-script.py), waits, does the given steps (those of
# gimp-devtools/gui/cdp.mjs) and leaves a screenshot of the page in
# tests/output/gui/look.png; then stops GIMP. For looking at the layout.
# Run tests/gimp-check.sh first.
#
#   tests/gui/look.sh exposures|focus|align [step...]
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
here=$(cd "$(dirname "$0")" && pwd)
GUI_MODE=$1
shift
export GUI_MODE
# shellcheck source=SCRIPTDIR/common.sh
. "$here/common.sh"
start_gimp look
sleep 12
# shellcheck disable=SC2086,SC2068
$cdp "$view" $@ wait:500 shot:"$out/look.png" >/dev/null || exit 1
touch "$out/check" "$out/done"
echo "$out/look.png"
