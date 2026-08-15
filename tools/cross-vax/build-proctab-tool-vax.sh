#!/bin/sh
# build-proctab-tool-vax.sh - cross-compile + link-prove the process-table
# guest tool (tests/netbsd/guest/vmsproctab.c) for netbsd-vax (rd vms-2e0,
# parent vms-945e, epic vms-8e8).
#
# FAST PER-PR GATE, no NBSRC / no SIMH. The nightly runtime proof
# (tests/lab-vax/run-proctab.sh, drive_proctab_vax.py) boots real NetBSD/vax
# under SIMH to prove $GETJPI/$PROCESS_SCAN cross-process semantics against a
# live /dev/vms; that is slow and disk-heavy. This script is the fast per-PR
# complement, the vax analogue of build-eflag-tool-vax.sh: it cross-compiles
# vmsproctab.c against the NetBSD/vax kif_transport leaf
# (src/libvmssys/kif_transport_netbsd.c) and LINKS a real vax--netbsdelf
# ELF32 executable against NetBSD libc. No kernel headers are needed --
# vmsproctab.c is ordinary userspace code, same as vmseflag.c (P4-E). The
# point (mirrors rd vms-4e7's for event flags): vax is ILP32 /
# non-IEEE-float / ELF32, so a struct-layout or width bug in the shared
# vms_proctab_nb.h wire contract (src/kernel-netbsd/vms_proctab_nb.h) can
# compile clean on every 64-bit OVMX target and only break here -- this gate
# catches that on every PR, fast.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile).
#
# ENV:
#   CROSSCOMPILE_NEGCTL=1   teeth check: a deliberately-broken vmsproctab.c
#                           width assumption must FAIL the vax cross-compile.
#
# Clean-room (CLAUDE.md Rule 8): OVMX's own build glue over public NetBSD
# headers + a stock gcc. No NetBSD or VSI source is copied.

set -eu

TARGET="${TARGET:-vax--netbsdelf}"
SYSROOT="${SYSROOT:-/opt/cross/sysroot}"
CC="${TARGET}-gcc"
SRC="$(pwd)"
LIBVMSSYS="$SRC/src/libvmssys"
KMOD="$SRC/src/kernel-netbsd"
PROBE="$SRC/tests/netbsd/guest"
OUT="${OUT:-/tmp/vax-proctab-tool-build}"
rm -rf "$OUT"; mkdir -p "$OUT"

echo "=== toolchain ==="
"$CC" --version | head -1
"$CC" -dumpmachine
echo

# --- negative control (teeth) -------------------------------------------------
# Mirrors build-eflag-tool-vax.sh's width negctl, but against the actual wire
# struct vmsproctab.c depends on (struct vms_procinfo carries vms_pid as
# uint32_t): a TU that assumes the field is a 64-bit `long' (an LP64-ism no
# ILP32 vax build would ever produce from the real header) must be REJECTED.
if [ "${CROSSCOMPILE_NEGCTL:-0}" = "1" ]; then
    echo "=== NEGATIVE CONTROL: a deliberately-broken proctab TU must FAIL the vax cross-compile ==="
    cat > "$OUT/proctab_negctl.c" <<'EOF'
#include <stdint.h>
/* Deliberately WRONG: pretends the process-table row's vms_pid is a 64-bit
 * long (an LP64 assumption). On vax (ILP32) long is 32-bit, so this static
 * assertion must be a hard compile error -- if it is not, the width gate this
 * probe depends on (vms_proctab_nb.h's uint32_t vms_pid/uic fields) has no
 * teeth. */
_Static_assert(sizeof(long) == 8, "NEGCTL: pretend vax long is 64-bit (LP64)");
int main(void){ return 0; }
EOF
    if "$CC" --sysroot="$SYSROOT" -c "$OUT/proctab_negctl.c" -o "$OUT/proctab_negctl.o" 2>"$OUT/negctl.err"; then
        echo "FAIL (negctl): the LP64 width assertion compiled clean on vax -- no teeth"
        exit 1
    fi
    echo "--- rejected as expected (compiler diagnostics) ---"
    grep -iE 'static.?assert|error' "$OUT/negctl.err" | head -4 || true
    echo "PASS (negctl): the LP64 width assumption was REJECTED by the vax--netbsdelf cross-compile"
    exit 0
fi

# --- the real proof: cross-compile + link vmsproctab for elf32-vax -----------
echo "=== cross-compile + link vmsproctab.c for $TARGET ==="
test -f "$PROBE/vmsproctab.c" || { echo "FAIL: $PROBE/vmsproctab.c not found"; exit 1; }
test -f "$KMOD/vms_proctab_nb.h" || { echo "FAIL: $KMOD/vms_proctab_nb.h not found"; exit 1; }

"$CC" --sysroot="$SYSROOT" -O -Wall -Wextra -static \
    -I"$LIBVMSSYS" -I"$KMOD" \
    -o "$OUT/vmsproctab" \
    "$PROBE/vmsproctab.c" "$LIBVMSSYS/kif_transport_netbsd.c"

echo "--- linked executable ---"
file "$OUT/vmsproctab" 2>/dev/null || true
"$TARGET-objdump" -f "$OUT/vmsproctab" | grep -Ei 'file format|architecture'
"$TARGET-readelf" -h "$OUT/vmsproctab" | grep -Ei 'Class|Data|Machine|Type'
"$TARGET-objdump" -f "$OUT/vmsproctab" | grep -qiF 'file format elf32-vax' \
    || { echo "FAIL: vmsproctab was not built elf32-vax"; exit 1; }

echo
echo "ALL PROOFS PASSED: vmsproctab (process-table guest tool) builds and links for $TARGET"
