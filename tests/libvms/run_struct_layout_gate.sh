#!/bin/sh
# run_struct_layout_gate.sh — vms-801.5 non-fakeness proof for the VMS ABI
# struct layout-stability gate (tests/libvms/test_conformance_layout.c).
#
# A compile-time _Static_assert gate is only real if it can FAIL.  This runner
# proves it:
#   POSITIVE: compile the audit as-is                    -> MUST succeed.
#   NEGATIVE: compile with -DLAYOUT_GATE_INJECT_DRIFT     -> MUST fail (the
#             deliberately-drifted ILE3 shifts an offset the pins reject).
# If the positive fails, or the negative compiles, the gate is broken.
#
# POSIX sh — no bashisms (no `set -o pipefail`, no arrays, no [[ ]]); this runs
# under dash when ctest invokes /bin/sh.
#
# Args (from tests/libvms/CMakeLists.txt):
#   $1 = C compiler        $2 = source file
#   $3 = libvms include dir  $4 = vmsrms include dir
set -u

CC=${1:?compiler}
SRC=${2:?source}
INC_LIBVMS=${3:?libvms include dir}
INC_RMS=${4:?vmsrms include dir}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/struct_layout_gate.XXXXXX") || {
    echo "FAIL: mktemp"; exit 1; }
trap 'rm -rf "$WORK"' EXIT

CFLAGS="-std=gnu11 -O0 -c -I$INC_LIBVMS -I$INC_RMS"

echo "== POSITIVE: audit must compile clean =="
if $CC $CFLAGS -o "$WORK/pos.o" "$SRC" 2>"$WORK/pos.err"; then
    echo "  ok: audit compiled"
else
    echo "FAIL: the layout audit did not compile clean (a real layout drift?):"
    cat "$WORK/pos.err"
    exit 1
fi

echo "== NEGATIVE: drifted layout MUST fail to compile =="
if $CC $CFLAGS -DLAYOUT_GATE_INJECT_DRIFT -o "$WORK/neg.o" "$SRC" 2>"$WORK/neg.err"; then
    echo "FAIL: the drifted layout compiled — the gate cannot fail, so it is FAKE."
    exit 1
else
    echo "  ok: drifted layout rejected at compile time (gate proven live)"
    # Confirm it failed for the RIGHT reason (our _Static_assert), not some
    # unrelated compile error that would make the negative pass spuriously.
    if grep -q "DRIFT PROOF" "$WORK/neg.err"; then
        echo "  ok: failure is the DRIFT PROOF _Static_assert"
    else
        echo "FAIL: negative compile failed, but NOT via the DRIFT PROOF assertion:"
        cat "$WORK/neg.err"
        exit 1
    fi
fi

echo "STRUCT LAYOUT GATE PROVEN: passes clean, rejects drift."
exit 0
