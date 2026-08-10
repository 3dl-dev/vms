#!/bin/sh
#
# test_guide_drift.sh - the install/upgrade guides cannot silently drift from
# the e2e gates that actually prove them (vms-55a, epic vms-a84 RELEASE
# ENGINEERING).
#
# WHAT THIS GUARDS AGAINST. docs/install-guide.md and docs/upgrade-guide.md
# document a real DCL procedure. Before this bead, nothing checked that the
# procedure a human reads still matches what tests/qemu/
# test_product_install_e2e.sh / tests/qemu/test_upgrade_e2e.sh actually run
# under QEMU -- a later edit to either the gate or the guide could silently
# diverge and nobody would notice until an operator typed the guide's
# commands and hit an error the gate would have caught.
#
# THE MECHANISM: tools/check_guide_drift.py diffs, in order, the
# '# GUIDE-STEP'-annotated `send '...'` commands in a gate script against the
# guide's own <!-- ovmx:guide-steps:begin/end --> fenced code block. See that
# script's header for the full reasoning. This is a pure source/text-scan
# gate -- no docker, no QEMU, cheap enough to run on every build.
#
# See test_guide_drift_negctl.sh for the negative control that proves this
# comparator can actually go red on real drift, not just report OK no matter
# what it is handed.
#
# Usage: test_guide_drift.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
CHECK="$SRC_ROOT/tools/check_guide_drift.py"
status=0

command -v python3 >/dev/null 2>&1 || { echo "FAIL: python3 not available"; exit 1; }
[ -f "$CHECK" ] || { echo "FAIL: $CHECK missing"; exit 1; }

echo "Guide-drift gate: install-guide.md / upgrade-guide.md vs. their e2e gates"

check_pair() {  # check_pair <gate> <guide>
    gate="$1"; guide="$2"
    out=$(python3 "$CHECK" --gate "$gate" --guide "$guide" 2>&1)
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "  PASS: $out"
    else
        echo "  FAIL: $guide has drifted from $gate (or a fixture problem -- see below)"
        echo "$out" | sed 's/^/          | /'
        status=1
    fi
}

check_pair "$SRC_ROOT/tests/qemu/test_product_install_e2e.sh" "$SRC_ROOT/docs/install-guide.md"
check_pair "$SRC_ROOT/tests/qemu/test_upgrade_e2e.sh" "$SRC_ROOT/docs/upgrade-guide.md"

exit "$status"
