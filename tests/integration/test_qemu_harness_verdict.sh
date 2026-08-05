#!/bin/bash
# test_qemu_harness_verdict.sh - controls for the QEMU harness verdict (rd vms-b1f)
#
# The verdict tests/qemu/run_tests.sh reaches -- "did this run report zero
# failed suites?" -- used to be computed by a pipeline under `set -o pipefail`,
# which inverted it on any transcript carrying more than one pipe buffer of
# output AFTER the matching line. See tests/qemu/lib/harness_verdict.sh for the
# isolated measurement.
#
# These controls EXECUTE the verdict against synthetic transcripts. They do not
# boot QEMU and they do not grep the harness for the shape of the old bug --
# enumerating the shape of a defect is not the same as proving the defect is
# gone, and a later rewrite could reintroduce it in a form no pattern names.
#
# Control 1 carries its own DISPROOF: on the same input that the fixed verdict
# gets right, it runs the OLD pipeline form and asserts it gets it WRONG. If
# that ever stops being wrong, control 1 has stopped testing anything and says
# so, rather than passing quietly.

set -uo pipefail

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
HELPER="$ROOT/tests/qemu/lib/harness_verdict.sh"
RUNNER="$ROOT/tests/qemu/run_tests.sh"

passed=0
failed=0

ok()   { echo "  PASS: $1"; passed=$((passed + 1)); }
bad()  { echo "  FAIL: $1"; failed=$((failed + 1)); }

echo "QEMU harness verdict controls (rd vms-b1f)"
echo ""

if [ ! -r "$HELPER" ]; then
    echo "FAIL: REFUSING to certify: no helper at $HELPER"
    exit 2
fi
# shellcheck source=/dev/null
. "$HELPER"

WORK=$(mktemp -d) || { echo "FAIL: mktemp -d failed" >&2; exit 2; }
trap 'rm -f "$WORK"/*; rmdir "$WORK" 2>/dev/null || true' EXIT

# One pipe buffer is ~64 KiB; 200 KB is comfortably past it. This is the whole
# point of the control -- a SMALL transcript never reproduced the defect, which
# is why it survived so long.
BIG=$(head -c 200000 /dev/zero | tr '\0' 'x')

# --- 1. THE REGRESSION: zero failures, with a pipe buffer of trailing noise ---
printf 'FINAL RESULTS: 5 suites run, 0 suites failed\n%s\n' "$BIG" > "$WORK/big_zero"
if harness_verdict_zero_failures "$WORK/big_zero"; then
    ok "zero failures + 200KB of trailing output is a PASS"
else
    bad "zero failures + 200KB of trailing output was reported as a FAILURE"
    echo "        This is rd vms-b1f: the harness reporting KERNEL MODULE TESTS"
    echo "        FAILED on a run with zero failures. Do NOT fix it by removing"
    echo "        pipefail; remove the pipeline."
fi

# --- 1b. DISPROOF: the old form must still get this wrong -------------------
# Proves control 1 is load-bearing rather than passing for an unrelated reason.
( set -o pipefail; echo "$(cat "$WORK/big_zero")" | grep -qE "$HARNESS_VERDICT_ZERO_RE" ) \
    >/dev/null 2>&1
old_status=$?
if [ "$old_status" -ne 0 ]; then
    ok "disproof: the old piped form still gets control 1's input WRONG (status $old_status)"
else
    bad "disproof: the old piped form now gets control 1's input RIGHT"
    echo "        Control 1 is no longer discriminating: this input no longer"
    echo "        reproduces the defect, so passing it proves nothing. Grow the"
    echo "        trailing output until it does, or retire the control with an"
    echo "        argument -- do not leave it passing vacuously."
fi

# --- 2. a nonzero count is a FAILURE ----------------------------------------
printf 'FINAL RESULTS: 5 suites run, 2 suites failed\n%s\n' "$BIG" > "$WORK/big_two"
if harness_verdict_zero_failures "$WORK/big_two"; then
    bad "'2 suites failed' was reported as a PASS"
else
    ok "'2 suites failed' is a FAILURE"
fi

# --- 3. the [^0-9] property: a count ENDING in zero is not zero -------------
# Latent from the day the harness was written; it first fired when the
# executive-absent failure count reached double digits.
printf 'FINAL RESULTS: 25 suites run, 10 suites failed\n' > "$WORK/ten"
if harness_verdict_zero_failures "$WORK/ten"; then
    bad "'10 suites failed' was reported as a PASS"
    echo "        The [^0-9] guard in HARNESS_VERDICT_ZERO_RE has been lost."
else
    ok "'10 suites failed' is a FAILURE (the [^0-9] guard holds)"
fi

# --- 4. no summary line at all is a FAILURE, not a pass ---------------------
printf 'boot: something went wrong before the summary\n%s\n' "$BIG" > "$WORK/nosum"
if harness_verdict_zero_failures "$WORK/nosum"; then
    bad "a transcript with NO 'FINAL RESULTS' line was reported as a PASS"
    echo "        A run that died before printing its own summary has not"
    echo "        reported success. Treating an absent line as 'nothing failed'"
    echo "        is the silent fallback CLAUDE.md Rule 9 forbids."
else
    ok "a transcript with no summary line is a FAILURE"
fi

# --- 5. a missing transcript is a REFUSAL, distinct from a failure ----------
harness_verdict_zero_failures "$WORK/does_not_exist" 2>/dev/null
rc=$?
if [ "$rc" -eq 2 ]; then
    ok "a missing transcript is a REFUSAL (rc=2), not a verdict"
else
    bad "a missing transcript returned rc=$rc, not the refusal rc=2"
fi

# --- 6. OVER-FIRING BOUND: a small clean transcript still passes ------------
# A gate that reds on correct input is the one the next person weakens.
printf 'FINAL RESULTS: 5 suites run, 0 suites failed\n' > "$WORK/small_zero"
if harness_verdict_zero_failures "$WORK/small_zero"; then
    ok "a small clean transcript is still a PASS (over-firing bound)"
else
    bad "a small clean transcript was reported as a FAILURE"
fi

# --- 7. the helper is actually the one run_tests.sh uses --------------------
# Without this, the helper could be correct and dead while the harness kept its
# own copy of the defect.
if [ ! -r "$RUNNER" ]; then
    bad "no runner at $RUNNER to check"
elif grep -q 'harness_verdict_zero_failures' "$RUNNER" \
     && grep -q 'lib/harness_verdict.sh' "$RUNNER"; then
    ok "run_tests.sh sources the helper and calls it"
else
    bad "run_tests.sh does not both source the helper and call it"
    echo "        A correct helper that nothing uses is not a fix."
fi

echo ""
echo "  controls: $passed passed, $failed failed"
if [ "$failed" -ne 0 ]; then
    echo "QEMU harness verdict: FAIL"
    exit 1
fi
echo "QEMU harness verdict: PASS"
exit 0
