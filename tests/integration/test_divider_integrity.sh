#!/bin/sh
#
# test_divider_integrity.sh - wire tools/check_divider_integrity.py (rd
# vms-6d7) into ctest as a real gate, plus run its own proof suite.
#
# WHY THIS EXISTS: vms-371's rebase (2e30da9, squash-merged as 2dcd237)
# silently corrupted 3 comment-divider blocks in
# tools/cluster/scs_join_capability_measure.py -- a box-divider line lost its
# trailing newline and fused onto the next comment line, with the divider run
# itself truncated (68 vs 75 chars), OUTSIDE the region git reported as
# needing manual merge resolution. It sat on main ~30 minutes undetected:
# py_compile only checks syntax and this repo's wire/data tests validate
# logic via string search on a markdown spec file, never comment formatting.
# That instance is already fixed (vms-beb, a21df8a / 155a820) -- this gate is
# the mechanical safeguard against the CLASS recurring, not a re-fix.
#
# TWO THINGS RUN HERE:
#
#   1. THE PROOF SUITE (tests/integration/test_divider_integrity_selftest.py)
#      -- proves the detector actually catches the bug's exact reported
#      shape on synthetic fixtures and does not false-positive on this
#      repo's real divider/banner styles. This is what makes (2) trustworthy.
#
#   2. THE GATE ITSELF -- run tools/check_divider_integrity.py over every
#      tracked .py/.c/.h file under src/, tools/, tests/. Any finding is a
#      RED here. (The proof suite's own repo-tree test (2) duplicates this
#      narrowly against the same file set for its own reasons; running it
#      again here, standalone, is what makes THIS test -- not just the
#      selftest file -- the thing CI actually gates on.)
#
# Needs python3. A missing interpreter is registered as a RED (not a silent
# skip) for the same reason tests/vmsscs/*_no_python3.cmake are: a skipped
# test is a failing test (project rule 7), and this check exists precisely
# because nothing else in the suite would catch this corruption class.
#
# Usage: test_divider_integrity.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
CHECKER="$SRC_ROOT/tools/check_divider_integrity.py"
SELFTEST="$SRC_ROOT/tests/integration/test_divider_integrity_selftest.py"

echo "divider integrity: catching git 3-way-merge comment-divider corruption (vms-6d7)"

if ! command -v python3 >/dev/null 2>&1; then
    echo "FAIL: no python3 interpreter on PATH."
    echo "      This is the only check in the suite that catches a git 3-way"
    echo "      auto-merge silently truncating/fusing a comment-divider block"
    echo "      (the vms-371 rebase incident) -- it cannot be skipped quietly."
    exit 1
fi

if [ ! -f "$CHECKER" ]; then
    echo "FAIL: checker not found at $CHECKER"
    exit 1
fi

if [ ! -f "$SELFTEST" ]; then
    echo "FAIL: proof suite not found at $SELFTEST"
    exit 1
fi

status=0

echo
echo "  1. proof suite: does the detector catch the bug's shape without"
echo "     false-positiving on this repo's real divider/banner styles?"
if ! python3 "$SELFTEST" -v; then
    echo "FAIL: the detector's own proof suite failed -- the detector is not"
    echo "      trustworthy, so the gate below is not run."
    exit 1
fi

echo
echo "  2. the gate: sweep tracked .py/.c/.h under src/, tools/, tests/"
COUNT=$(cd "$SRC_ROOT" && find src tools tests \
    -type d \( -name .git -o -name __pycache__ \) -prune -o \
    -type f \( -name '*.py' -o -name '*.c' -o -name '*.h' \) -print0 2>/dev/null \
    | tr -dc '\0' | wc -c)

if [ "$COUNT" -eq 0 ]; then
    echo "FAIL: found no .py/.c/.h files under src/, tools/, tests/ -- the file"
    echo "      list is almost certainly wrong, not the tree actually empty."
    exit 1
fi

if ! ( cd "$SRC_ROOT" && find src tools tests \
        -type d \( -name .git -o -name __pycache__ \) -prune -o \
        -type f \( -name '*.py' -o -name '*.c' -o -name '*.h' \) -print0 2>/dev/null \
        | xargs -0 python3 "$CHECKER" ); then
    echo "FAIL: comment-divider corruption found in tracked source (see above)."
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo
    echo "divider integrity: PASS"
fi

exit "$status"
