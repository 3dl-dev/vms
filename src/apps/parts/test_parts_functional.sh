#!/bin/sh
# test_parts_functional.sh - host functional gate for the PARTS RMS demo
# (beads vms-e97 / vms-f20). Runs the dev PARTS executable against a scratch
# indexed file and asserts the demo's load + keyed-random-lookup behaviour.
#
# This proves the RMS application LOGIC on the host. The VMS-native activation
# proof (PARTS.EXE via LINK.EXE/IMGACT under QEMU) is a separate gate,
# tests/qemu/test_parts_rms_qemu.sh.
#
# Usage: test_parts_functional.sh <path-to-PARTS>
set -u

PARTS="${1:?usage: test_parts_functional.sh <PARTS-binary>}"
[ -x "$PARTS" ] || { echo "FAIL: PARTS binary not executable: $PARTS"; exit 1; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/parts-func.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT

SPEC="$WORK/PARTS.DAT"
COUNT=250

echo "=== PARTS functional test: load $COUNT, keyed lookups ==="
OUT=$(PARTS_FILE="$SPEC" PARTS_COUNT="$COUNT" "$PARTS" 2>&1)
RC=$?
echo "$OUT"
echo "--- (exit $RC) ---"

fail=0
assert() {
    desc="$1"; pat="$2"
    if printf '%s\n' "$OUT" | grep -qF "$pat"; then
        echo "  PASS: $desc"
    else
        echo "  FAIL: $desc (missing: $pat)"
        fail=1
    fi
}

# Assert a key is FOUND: the "-S-FOUND  key <k>" line must be present.
assert_found() {
    k="$1"
    if printf '%s\n' "$OUT" | grep -qF "%PARTS-S-FOUND  key $k"; then
        echo "  PASS: keyed lookup FOUND $k"
    else
        echo "  FAIL: keyed lookup did not FIND $k"
        fail=1
    fi
}

[ "$RC" -eq 0 ] || { echo "  FAIL: PARTS exited non-zero ($RC)"; fail=1; }
assert "indexed file created"                 "%PARTS-I-CREATE"
assert "records loaded"                       "%PARTS-I-LOADED, $COUNT part records"
assert_found "PN000001"                        # first
assert_found "PN000125"                        # middle (COUNT/2)
assert_found "PN000250"                        # last (COUNT) - the tail record
assert "never-loaded key reported NOTFOUND"     "%PARTS-W-NOTFOUND, no part with key PN999999"
assert "demo reports all lookups correct"       "%PARTS-S-DONE"
if printf '%s\n' "$OUT" | grep -qF "%PARTS-E-VERIFY"; then
    echo "  FAIL: a keyed lookup returned the wrong result (%PARTS-E-VERIFY)"; fail=1
fi

# The indexed file and its B-tree sidecar must actually exist on disk.
if ls "$WORK"/PARTS.DAT* >/dev/null 2>&1; then
    echo "  PASS: indexed data file present on disk"
else
    echo "  FAIL: no PARTS.DAT written"
    fail=1
fi
if ls "$WORK"/*.rms_idx >/dev/null 2>&1; then
    echo "  PASS: B-tree index sidecar present on disk"
else
    echo "  FAIL: no .rms_idx sidecar (index not built)"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "=== ALL PARTS FUNCTIONAL CHECKS PASSED ==="
    exit 0
else
    echo "=== PARTS FUNCTIONAL CHECKS FAILED ==="
    exit 1
fi
