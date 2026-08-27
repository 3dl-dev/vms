#!/bin/sh
#
# kernel_executive_negctl_crash.sh - negative control on the vms-c9c
# diagnostic (rd vms-a3c rework).
#
# WHAT SHIPPED, AND WHAT IT BROKE. vms-c9c (commit 74e3200) replaced a hand-
# typed inferred cause with `suite_fails=$(... | grep -F "FAIL:")` in
# .github/workflows/ci.yml's kernel-executive-negative-control step. Both
# shapes the vms-c9c implementer exercised (an ordinary assertion failure,
# and the clean pass) emit a FAIL: line for the suite in question, so grep
# always matched and the pipeline always exited 0. The THIRD shape -- a
# test_syssvc_* suite that CRASHES (segfault, killed by a signal, any exit
# with no FAIL: line at all -- rc=139/141/etc are real data per CLAUDE.md
# Rule 9, not noise) -- was never run. grep -F exits 1 on no match; this
# step runs under `bash -e` (set at the top of the step), and a bare
# `var=$(cmd)` assignment is NOT one of the errexit-exempt forms -- so the
# unguarded grep ABORTED THE ENTIRE STEP the instant a suite crashed,
# silently: no per-suite verdicts, no "FAIL: negative control did not go red
# for the RIGHT REASON", the vms.ko module-load check never
# reached. The step meant to make the diagnostic honest instead went silent
# in exactly the case that most needs a diagnostic.
#
# WHAT THIS FILE PROVES, AND HOW. It does not boot QEMU or touch /dev/vms --
# it needs neither. tools/replay_ci_kernel_executive.py extracts the
# `kernel-executive-negative-control` step's `run:` block VERBATIM out of
# .github/workflows/ci.yml and executes it under `bash -e -c` against a
# supplied captured-output file, so whatever this script asserts is
# asserted against the SAME TEXT CI runs, not a hand-copied approximation
# that could drift from it. This file builds ONE synthetic capture with the
# crash shape (a real test_syssvc_* suite from the current checkout, rc=139,
# no FAIL: line anywhere in its section) and checks that the step:
#
#   1. does not abort before reaching its own final accounting -- the
#      opposite of the vms-c9c bug, which produced NO diagnostic text at all;
#   2. names the crashed suite and its rc in what it prints;
#   3. says so explicitly when a suite prints no FAIL: line, rather than
#      pasting nothing after a colon;
#   4. does NOT print the unhedged "means EITHER ... OR ... say which"
#      dichotomy vms-a3c found asserted as fact -- the printed text must be
#      hedged (matching the source comment's own "at least two ... causes"),
#      because a suite that crashes before printing anything is a THIRD
#      shape the dichotomy did not name.
#
# Run this file against a checkout with the vms-c9c bug still present (the
# `|| true` removed from the `grep -F "FAIL:"` pipeline) and check 1 fails:
# the replay exits nonzero with output that stops right after "Derived
# expected suites from the checkout (N):" and never reaches a verdict line.
# That is the reproduction this file exists to keep reproduced.
#
# Usage: kernel_executive_negctl_crash.sh [<repo-root>]

set -u

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
REPLAY="$ROOT/tools/replay_ci_kernel_executive.py"
WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT INT TERM

passed=0
failed=0
ok()  { echo "  ok: $*"; passed=$((passed + 1)); }
bad() { echo "  FAIL: $*"; failed=$((failed + 1)); }

echo "=============================================================="
echo " Negative control: kernel-executive-negative-control step must"
echo " SURVIVE a crashing test_syssvc_* suite with a true diagnostic"
echo " (vms-a3c)"
echo "=============================================================="

[ -f "$REPLAY" ] || { echo "FAIL: BROKEN FIXTURE: $REPLAY is missing"; exit 1; }

# Ground the fixture in a REAL suite name from THIS checkout, derived the
# same way ci.yml derives EXPECTED, so this file cannot drift from what the
# step actually iterates over.
CRASH_SUITE=$(cd "$ROOT" && ls tests/qemu/test_syssvc_*.c 2>/dev/null \
              | xargs -n1 basename | sed 's/\.c$//' | sort | head -n1)
if [ -z "$CRASH_SUITE" ]; then
    echo "FAIL: BROKEN FIXTURE: no tests/qemu/test_syssvc_*.c found in $ROOT"
    exit 1
fi

ALL_SUITES=$(cd "$ROOT" && ls tests/qemu/test_kmod_*.c tests/qemu/test_syssvc_*.c 2>/dev/null \
             | xargs -n1 basename | sed 's/\.c$//' | sort)

CAPTURE="$WORKDIR/crash_capture.txt"
{
    echo "cannot open /dev/vms: No such file or directory"
    echo ""
    for suite in $ALL_SUITES; do
        case "$suite" in
            "$CRASH_SUITE")
                # THE INJECTED SHAPE: killed by SIGSEGV (128+11=139), no
                # FAIL: line anywhere -- the process died before it could
                # print one.
                echo "=== SUITE $suite rc=139 ==="
                ;;
            test_syssvc_*)
                echo "=== SUITE $suite rc=77 ==="
                ;;
            *)
                echo "FAIL: $suite raw ioctl unexpectedly worked without executive"
                echo "=== SUITE $suite rc=1 ==="
                ;;
        esac
    done
    echo "FAIL: vms.ko load or /dev/vms creation failed -- executive absent as expected"
    echo "=== FINAL RESULTS: 2 suites passed, 25 suites failed ==="
} > "$CAPTURE"

OUTPUT=$(python3 "$REPLAY" negative "$CAPTURE" 1 2>&1)
RC=$?

echo ""
echo "--- 1. the step reaches ITS OWN final accounting instead of aborting ---"
# Catches: exactly the vms-c9c bug. An unguarded `grep -F "FAIL:"` on the
# crashed suite's (empty) FAIL-line set exits 1, and under `bash -e` that
# aborts the whole block right after printing the derived EXPECTED list --
# BEFORE either terminal message below is ever reached.
case "$OUTPUT" in
    *"FAIL: negative control did not go red for the RIGHT REASON:"*) \
        ok "the step reached its own verdict block (did not abort mid-way)" ;;
    *"PASS: harness went red for the RIGHT REASON"*) \
        bad "the step reported PASS for a crashing suite -- should have flagged $CRASH_SUITE" ;;
    *) \
        bad "the step produced NEITHER terminal message -- this IS the vms-c9c silent-abort signature (rc=$RC)" ;;
esac

echo ""
echo "--- 2. the crashed suite and its rc are named in the diagnostic ---"
case "$OUTPUT" in
    *"$CRASH_SUITE: rc=139"*) ok "$CRASH_SUITE and rc=139 both appear in the printed diagnostic" ;;
    *) bad "the diagnostic never names $CRASH_SUITE with its rc=139 -- attribution lost" ;;
esac

echo ""
echo "--- 3. an explicit note replaces the FAIL-line paste when there is none ---"
case "$OUTPUT" in
    *"no FAIL: line was printed for this suite"*) ok "the empty-FAIL-line case is stated explicitly, not pasted as nothing" ;;
    *) bad "no explicit note for the empty-FAIL-line case -- a blank paste after the colon is silent, not honest" ;;
esac

echo ""
echo "--- 4. the printed enumeration is hedged, not the unproven exhaustive claim ---"
case "$OUTPUT" in
    *"means EITHER a device-absent assertion inside the suite failed"*"say which:"*) \
        bad "the OLD unhedged closed dichotomy is still being printed verbatim" ;;
    *) ok "the old unhedged 'EITHER ... OR ... say which' phrasing is gone" ;;
esac
case "$OUTPUT" in
    *"AT LEAST two known causes"*) ok "the printed text hedges, matching the source comment's own 'at least two ... causes'" ;;
    *) bad "the printed text does not hedge the cause count -- re-check the wording change" ;;
esac

echo ""
echo "=============================================================="
echo "kernel_executive_negctl_crash: $passed passed, $failed failed"
echo "=============================================================="
[ "$failed" -eq 0 ]
