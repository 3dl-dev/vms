#!/bin/bash
# Self-test for the DCL test harness (run_dcl_tests.sh).
#
# WHY THIS EXISTS (vms-6a7 round 2). The harness ran `grep -qF "$needle"` with
# no `--`, so any needle beginning with a dash was parsed by grep as an OPTION:
# grep exited 2 and the caller's `if grep ...` read that as "did not match". For
# an EXPECT_NOT that means the assertion PASSED UNCONDITIONALLY. The live
# casualty was `# EXPECT_NOT: contains:---` in test_show_system.sh — the guard
# that keeps CLAUDE.md Rule 10 placeholder markers out of SHOW SYSTEM. It was
# inert from the day it was written: a build that printed
# "PLACEHOLDER      ---   ---   ---" reported 90 passed, 0 failed.
#
# A one-character fix is invisible to every existing test, because every
# existing test still passes either way. So the fix is guarded here instead:
# this file drives the real harness against synthetic test files whose expected
# verdict is known, and fails if the harness gets any of them wrong.
#
# This file is deliberately NOT named test_*.sh — run_dcl_tests.sh globs
# test_*.sh, and the self-test must not be collected by the harness it tests.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HARNESS="$SCRIPT_DIR/run_dcl_tests.sh"

if [ ! -f "$HARNESS" ]; then
    echo "SELFTEST ERROR: harness not found at $HARNESS"
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The harness insists on a usable VMSDCL even for tests that never call it.
export VMSDCL=/bin/echo

# The harness looks for its cases next to itself, so give the copy its own dir.
cp "$HARNESS" "$WORK/run_dcl_tests.sh"

FAILURES=0

# Emit a synthetic case: name, annotation block, body.
mk_case() {
    local name="$1" annotations="$2" body="$3"
    {
        echo '#!/bin/bash'
        echo "# TEST: $name"
        printf '%s\n' "$annotations"
        printf '%s\n' "$body"
    } > "$WORK/test_${name}.sh"
}

# Run the harness over exactly one synthetic case and report PASS or FAIL.
verdict_of() {
    local name="$1"
    local out
    out=$(bash "$WORK/run_dcl_tests.sh" 2>&1)
    if printf '%s\n' "$out" | grep -q "^FAIL: $name"; then
        echo FAIL
    elif printf '%s\n' "$out" | grep -q "^PASS: $name"; then
        echo PASS
    else
        printf '%s\n' "$out" >&2
        echo UNKNOWN
    fi
}

check() {
    local name="$1" want="$2" annotations="$3" body="$4"
    rm -f "$WORK"/test_*.sh
    mk_case "$name" "$annotations" "$body"
    local got
    got="$(verdict_of "$name")"
    if [ "$got" = "$want" ]; then
        echo "ok   — $name: harness said $got (expected $want)"
    else
        echo "NOT OK — $name: harness said $got, expected $want"
        FAILURES=$((FAILURES + 1))
    fi
}

echo "=== DCL harness self-test ==="

# 1. The exact live casualty: a dash-leading EXPECT_NOT needle that IS present
#    must fail. Before the fix this reported PASS.
check dashneedle_present FAIL \
    '# EXPECT_NOT: contains:---' \
    'echo "PLACEHOLDER      ---   ---   ---"'

# 2. ...and the same needle must still pass when genuinely absent, so the fix
#    is not just "dash needles always fail".
check dashneedle_absent PASS \
    '# EXPECT_NOT: contains:---' \
    'echo "  Pid    Process Name           CPU"'

# 3. A dash-leading POSITIVE needle that is present must pass. Before the fix
#    grep exited 2 here too, which the harness read as "does not contain" — a
#    spurious failure rather than a silent one.
check dashneedle_positive PASS \
    '# EXPECT: contains:-VMS-F-' \
    'echo "%SYSTEM-VMS-F-NOPRIV, insufficient privilege"'

# 4. Same hole on the regex branch.
check dashregex_present FAIL \
    '# EXPECT_NOT: regex:-{3}' \
    'echo "row ---"'

# 5. A pattern grep cannot evaluate at all must fail loudly, not be folded into
#    "did not match". An unevaluatable EXPECT_NOT is an assertion that can never
#    go red, which is the whole defect class this file guards.
check bad_regex FAIL \
    '# EXPECT_NOT: regex:[unclosed' \
    'echo "anything"'

echo ""
if [ "$FAILURES" -eq 0 ]; then
    echo "=== harness self-test: all checks passed ==="
    exit 0
fi
echo "=== harness self-test: $FAILURES check(s) failed ==="
exit 1
