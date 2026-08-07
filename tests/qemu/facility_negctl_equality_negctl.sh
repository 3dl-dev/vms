#!/bin/sh
#
# facility_negctl_equality_negctl.sh - negative control ON the (suite,
# assertion-text) identity fix (rd vms-b3b).
#
# WHAT IS UNDER TEST, AND WHAT IS NOT. facility_negctl_equality.sh's
# fne_scope_map() is a pure text filter: given "<suite>\t<text>" lines (the
# exact shape run_facility_negctl.sh's fail_map() emits) and a defect's
# suites_red globs, it drops any row whose suite is not in scope. It boots
# nothing, runs no container, and proves nothing about the executive. What it
# CAN be wrong about is the one property this item exists to fix: whether a
# same-text red from a suite OUTSIDE suites_red can still stand in for the
# suite the manifest actually named. That is exactly, and only, what this
# file checks -- no QEMU needed, in well under a second.
#
# THE FIXTURE IS GROUNDED, NOT INVENTED. It uses the real, measured collision
# (rd vms-b3b's own evidence): bind-client-no-register's require_fail names
# "child: a LOCAL flag set by the parent is NOT visible here (local clusters
# stay per-process)", expected from test_syssvc_ef_mproc.c (IN that defect's
# suites_red); the identical string is also printed, verbatim, by
# test_kmod_eflag_mproc.c (NOT in it). Checks 1 and 3 below pin those two
# facts against the real manifest and the real sources, so a future rewording
# of the collision away would fail this file loudly instead of leaving it
# testing a scenario that no longer exists.
#
# Usage: facility_negctl_equality_negctl.sh [<repo-root>]

set -u

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
MANIFEST="$ROOT/tests/qemu/facility_defects.sh"
EQUALITY_LIB="$ROOT/tests/qemu/facility_negctl_equality.sh"
DEFECT="bind-client-no-register"
RIGHT_SUITE="test_syssvc_ef_mproc"
WRONG_SUITE="test_kmod_eflag_mproc"
TEXT="child: a LOCAL flag set by the parent is NOT visible here (local clusters stay per-process)"

passed=0
failed=0

ok()  { echo "  ok: $*"; passed=$((passed + 1)); }
bad() { echo "  FAIL: $*"; failed=$((failed + 1)); }

echo "=========================================================="
echo " Negative control on the red-set equality's suite scoping (vms-b3b)"
echo "=========================================================="

for f in "$MANIFEST" "$EQUALITY_LIB"; do
    [ -f "$f" ] || { echo "FAIL: BROKEN FIXTURE: $f is missing"; exit 1; }
done
. "$EQUALITY_LIB"

echo ""
echo "--- 1. the grounding facts still hold in the real manifest and sources ---"
# Catches: this file testing a scenario that has been fixed away by rewording
# or by moving the text out of one of the two suites -- which would make
# checks 2/4 below pass for a reason that says nothing about the fix.
red_globs=$(sh "$MANIFEST" field "$DEFECT" suites_red)
case " $red_globs " in
    *" $RIGHT_SUITE "*) ok "$DEFECT's suites_red names $RIGHT_SUITE" ;;
    *) bad "$DEFECT's suites_red no longer names $RIGHT_SUITE -- re-ground this fixture: [$red_globs]" ;;
esac
# shellcheck disable=SC2086
if fne_matches_globs "$WRONG_SUITE" $red_globs; then
    bad "$WRONG_SUITE now matches suites_red -- the collision this file exercises is gone, re-ground it"
else
    ok "$WRONG_SUITE is still outside $DEFECT's suites_red"
fi
if grep -qF "$TEXT" "$ROOT/tests/qemu/$RIGHT_SUITE.c" 2>/dev/null; then
    ok "$RIGHT_SUITE.c still prints the shared text verbatim"
else
    bad "$RIGHT_SUITE.c no longer contains the shared text -- re-ground this fixture"
fi
if grep -qF "$TEXT" "$ROOT/tests/qemu/$WRONG_SUITE.c" 2>/dev/null; then
    ok "$WRONG_SUITE.c still prints the IDENTICAL text -- the collision is real, measured, current"
else
    bad "$WRONG_SUITE.c no longer contains the shared text -- re-ground this fixture"
fi

echo ""
echo "--- 2. BASELINE: the right suite's red satisfies the requirement ---"
# The correct-attribution case must still pass -- this file exists to prove
# the WRONG suite is refused, not that every suite is refused.
_baseline_scoped=$(printf '%s\t%s\n' "$RIGHT_SUITE" "$TEXT" | fne_scope_map "$red_globs")
if [ "$_baseline_scoped" = "$(printf '%s\t%s' "$RIGHT_SUITE" "$TEXT")" ]; then
    ok "a red from $RIGHT_SUITE (in suites_red) survives scoping -- the true positive is not refused"
else
    bad "a red from $RIGHT_SUITE (in suites_red) was dropped by scoping -- this would false-fail every genuine defect"
fi

echo ""
echo "--- 3. THE FIX: a same-text red from the WRONG suite no longer satisfies it ---"
# THE control this whole file exists for. Under the OLD (text-only) equality,
# this row would have satisfied require_fail's "$TEXT" line even though it
# was never printed by $RIGHT_SUITE. After scoping, it must be DROPPED --
# leaving the requirement unmet, which is what makes the driver report a MISS
# instead of a false PASS.
_wrong_scoped=$(printf '%s\t%s\n' "$WRONG_SUITE" "$TEXT" | fne_scope_map "$red_globs")
if [ -z "$_wrong_scoped" ]; then
    ok "a red from $WRONG_SUITE (NOT in suites_red) is dropped by scoping -- it can no longer satisfy require_fail"
else
    bad "a red from $WRONG_SUITE survived scoping: [$_wrong_scoped] -- the vms-b3b defect is NOT fixed"
fi

echo ""
echo "--- 4. end-to-end: the driver's own comparison shape rejects the wrong-suite case ---"
# Reproduces run_facility_negctl.sh's actual step-6 comparison (require_fail
# vs the scoped observed set) against a fabricated run where ONLY the wrong
# suite reddened -- the exact failure mode named in this item: the intended
# suite stayed green, an unrelated suite printing the same text took its
# place. The old equality would have reported this defect PASS; the fix must
# report the text MISSING.
_tmp=$(mktemp -d) || { bad "mktemp failed"; _tmp=""; }
if [ -n "$_tmp" ]; then
    sh "$MANIFEST" field "$DEFECT" require_fail  >"$_tmp/exp"
    sh "$MANIFEST" field "$DEFECT" knock_on_fail >>"$_tmp/exp"
    grep -v '^$' "$_tmp/exp" | sort -u >"$_tmp/exp2"
    printf '%s\t%s\n' "$WRONG_SUITE" "$TEXT" >"$_tmp/map"
    fne_scope_map "$red_globs" <"$_tmp/map" | sort -u >"$_tmp/map.scoped"
    cut -f2- "$_tmp/map.scoped" | sort -u >"$_tmp/obs"
    _miss=$(comm -23 "$_tmp/exp2" "$_tmp/obs")
    case "$_miss" in
        *"$TEXT"*)
            ok "the driver's own comparison reports the shared text MISSING when only $WRONG_SUITE reddened"
            ;;
        *)
            bad "the driver's comparison did NOT report the text missing -- the wrong-suite red still satisfied it"
            ;;
    esac
    rm -rf "$_tmp"
fi

echo ""
echo "=========================================================="
echo " $passed passed, $failed failed"
echo "=========================================================="
[ "$failed" -eq 0 ] && exit 0
exit 1
