#!/bin/sh
#
# test_guide_drift_negctl.sh - negative controls for the guide-drift gate
# (vms-55a, epic vms-a84 RELEASE ENGINEERING).
#
# WHY THIS EXISTS. test_guide_drift.sh currently reports "OK" for both
# guides. A check that always reports OK, regardless of what it is handed,
# proves nothing -- this is the ANTI-LARP requirement the bead itself calls
# out: "if your tested guide isn't actually checked against the e2e gate in
# CI, it's just prose". These controls mutate a SANDBOX COPY of each real
# guide (never the tracked file) and require tools/check_guide_drift.py to
# go red, for the right reason, on every mutation.
#
# Usage: test_guide_drift_negctl.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
CHECK="$SRC_ROOT/tools/check_guide_drift.py"
INSTALL_GATE="$SRC_ROOT/tests/qemu/test_product_install_e2e.sh"
UPGRADE_GATE="$SRC_ROOT/tests/qemu/test_upgrade_e2e.sh"
INSTALL_GUIDE="$SRC_ROOT/docs/install-guide.md"
UPGRADE_GUIDE="$SRC_ROOT/docs/upgrade-guide.md"

status=0
passed=0
failed=0

command -v python3 >/dev/null 2>&1 || { echo "FAIL: python3 not available"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "Guide-drift gate negative controls: mutated guides must be caught, not certified"

# ---------------------------------------------------------------------------
# POSITIVE CONTROL. The real, unmutated guides must PASS. Without this, every
# "FAIL" below could mean the checker is just broken, not that it correctly
# caught the mutation.
# ---------------------------------------------------------------------------
if python3 "$CHECK" --gate "$INSTALL_GATE" --guide "$INSTALL_GUIDE" >/dev/null 2>&1 \
    && python3 "$CHECK" --gate "$UPGRADE_GATE" --guide "$UPGRADE_GUIDE" >/dev/null 2>&1; then
    echo "  PASS: positive control - the real, unmutated guides both pass the checker"
    passed=$((passed + 1))
else
    echo "  FAIL: positive control - a real, unmutated guide FAILS the checker, so no"
    echo "        negative control below can attribute a RED to its mutation"
    failed=$((failed + 1))
    status=1
fi

# expect_drift_caught <original-guide> <gate> <sed-expr> <name>
#   Copies <original-guide> to a sandbox file, applies <sed-expr>, and
#   requires the checker to exit 1 (drift) -- and to actually name the
#   mutated line's gate command in its diagnostic, proving it read real
#   content rather than just returning a fixed "drifted" verdict.
expect_drift_caught() {
    orig="$1"; gate="$2"; expr="$3"; name="$4"; need="$5"
    mutant="$WORK/$(basename "$orig").$$.mutant"
    cp "$orig" "$mutant"
    sed -i "$expr" "$mutant"

    if cmp -s "$orig" "$mutant"; then
        echo "  FAIL: BROKEN FIXTURE (not a broken gate): $name"
        echo "        the sed mutation did not change the file at all -- its anchor"
        echo "        no longer matches the guide, so this control proves nothing"
        failed=$((failed + 1))
        status=1
        return
    fi

    out=$(python3 "$CHECK" --gate "$gate" --guide "$mutant" 2>&1)
    rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -qF "DRIFTED"; then
        if [ -n "$need" ] && ! printf '%s\n' "$out" | grep -qF "$need"; then
            echo "  FAIL: $name - drifted (rc=1) but the diagnostic did not name the"
            echo "        expected content ($need) -- output may not reflect real content"
            echo "$out" | sed 's/^/          | /'
            failed=$((failed + 1))
            status=1
        else
            echo "  PASS: $name"
            passed=$((passed + 1))
        fi
    else
        echo "  FAIL: $name - expected exit 1 with a DRIFTED verdict, got rc=$rc:"
        echo "$out" | sed 's/^/          | /'
        failed=$((failed + 1))
        status=1
    fi
}

# ---------------------------------------------------------------------------
# INSTALL GUIDE: wrong command text on the very first step.
# ---------------------------------------------------------------------------
expect_drift_caught "$INSTALL_GUIDE" "$INSTALL_GATE" \
    's/MOUNT DKA100: WORK/MOUNT DKA100: WRONG/' \
    "install-guide: mutated device label caught as drift" \
    "MOUNT DKA100: WORK"

# INSTALL GUIDE: a step silently dropped (the DISMOUNT at the end).
expect_drift_caught "$INSTALL_GUIDE" "$INSTALL_GATE" \
    '/^\$ DISMOUNT DKA100:$/d' \
    "install-guide: dropped DISMOUNT step caught as drift" \
    "DISMOUNT DKA100:"

# ---------------------------------------------------------------------------
# UPGRADE GUIDE: the upgrade kit name quietly reverted to the baseline kit --
# exactly the class of silent error this gate exists to catch (a guide that
# tells an operator to "upgrade" onto the OLD kit).
# ---------------------------------------------------------------------------
expect_drift_caught "$UPGRADE_GUIDE" "$UPGRADE_GATE" \
    's/OVMX-OS-UPGRADE\.KIT/OVMX-OS-BASELINE.KIT/' \
    "upgrade-guide: kit name reverted to BASELINE caught as drift" \
    "OVMX-OS-UPGRADE.KIT"

# UPGRADE GUIDE: two steps reordered (kit carrier mounted after the target,
# instead of before) -- order matters and must be checked, not just membership.
expect_drift_caught "$UPGRADE_GUIDE" "$UPGRADE_GATE" \
    '1h;1!H;$!d;x;s/\$ MOUNT DKA200: KITS\n\$ MOUNT DKA100: WORK/$ MOUNT DKA100: WORK\n$ MOUNT DKA200: KITS/' \
    "upgrade-guide: MOUNT steps reordered caught as drift" \
    "MOUNT DKA200: KITS"

# ---------------------------------------------------------------------------
# FIXTURE-ERROR PATH: a guide with no GUIDE-STEPS block at all is a distinct
# failure mode (exit 2, "cannot even build the comparison") from real drift
# (exit 1) -- callers that treat any nonzero exit as "drift" would misreport
# this. Confirm the checker actually distinguishes them.
# ---------------------------------------------------------------------------
NO_BLOCK="$WORK/no-block-guide.md"
printf '# A guide with no marked steps\n\nJust prose, no fenced GUIDE-STEPS block.\n' > "$NO_BLOCK"
out=$(python3 "$CHECK" --gate "$INSTALL_GATE" --guide "$NO_BLOCK" 2>&1)
rc=$?
if [ "$rc" -eq 2 ]; then
    echo "  PASS: a guide with no GUIDE-STEPS block is a fixture error (exit 2), not a drift verdict"
    passed=$((passed + 1))
else
    echo "  FAIL: a guide with no GUIDE-STEPS block should exit 2 (fixture error), got rc=$rc:"
    echo "$out" | sed 's/^/          | /'
    failed=$((failed + 1))
    status=1
fi

echo ""
echo "=== Guide-drift gate negative controls: $passed passed, $failed failed ==="
exit "$status"
