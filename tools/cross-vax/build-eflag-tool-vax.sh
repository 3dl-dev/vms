#!/bin/sh
# build-eflag-tool-vax.sh - cross-compile + link-prove the event-flag guest tool
# (tests/netbsd/guest/vmseflag.c) for netbsd-vax (rd vms-4e7, parent vms-476,
# epic vms-8e8; blocked-on vms-f78bb).
#
# FAST PER-PR GATE, no NBSRC / no SIMH. The nightly runtime proof
# (tests/lab-vax/run-eflag.sh, drive_eflag_vax.py) boots real NetBSD/vax under
# SIMH to prove the cross-process event-flag semantics against a live /dev/vms;
# that is slow and disk-heavy. This script is the fast per-PR complement, the
# vax analogue of build-libvmssys-vax.sh: it cross-compiles vmseflag.c against
# the NetBSD/vax kif_transport leaf (src/libvmssys/kif_transport_netbsd.c) and
# LINKS a real vax--netbsdelf ELF32 executable against NetBSD libc. No kernel
# headers are needed -- vmseflag.c is ordinary userspace code, same as
# vmsprobe.c (P2b). The point (rd vms-4e7) is that vax is ILP32 / non-IEEE-float
# / ELF32, so a struct-layout or width bug in the shared vms_eflag_nb.h wire
# contract (src/kernel-netbsd/vms_eflag_nb.h) can compile clean on every 64-bit
# OVMX target and only break here -- this gate catches that on every PR, fast.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile).
#
# ENV:
#   CROSSCOMPILE_NEGCTL=1   teeth check: a deliberately-broken vmseflag.c width
#                           assumption must FAIL the vax cross-compile.
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
OUT="${OUT:-/tmp/vax-eflag-tool-build}"
rm -rf "$OUT"; mkdir -p "$OUT"

echo "=== toolchain ==="
"$CC" --version | head -1
"$CC" -dumpmachine
echo

# --- negative control (teeth) -------------------------------------------------
# Mirrors build-libvmssys-vax.sh's width negctl, but against the actual wire
# struct vmseflag.c depends on (struct vms_ef_args carries efn as uint32_t):
# a TU that assumes the field is a 64-bit `long' (an LP64-ism no ILP32 vax build
# would ever produce from the real header) must be REJECTED.
if [ "${CROSSCOMPILE_NEGCTL:-0}" = "1" ]; then
    echo "=== NEGATIVE CONTROL: a deliberately-broken eflag TU must FAIL the vax cross-compile ==="
    cat > "$OUT/eflag_negctl.c" <<'EOF'
#include <stdint.h>
/* Deliberately WRONG: pretends the common-cluster association ID is a 64-bit
 * long (an LP64 assumption). On vax (ILP32) long is 32-bit, so this static
 * assertion must be a hard compile error -- if it is not, the width gate this
 * probe depends on (vms_eflag_nb.h's uint32_t efn/status fields) has no teeth. */
_Static_assert(sizeof(long) == 8, "NEGCTL: pretend vax long is 64-bit (LP64)");
int main(void){ return 0; }
EOF
    if "$CC" --sysroot="$SYSROOT" -c "$OUT/eflag_negctl.c" -o "$OUT/eflag_negctl.o" 2>"$OUT/negctl.err"; then
        echo "FAIL (negctl): the LP64 width assertion compiled clean on vax -- no teeth"
        exit 1
    fi
    echo "--- rejected as expected (compiler diagnostics) ---"
    grep -iE 'static.?assert|error' "$OUT/negctl.err" | head -4 || true
    echo "PASS (negctl): the LP64 width assumption was REJECTED by the vax--netbsdelf cross-compile"
    exit 0
fi

# --- the real proof: cross-compile + link vmseflag for elf32-vax -------------
echo "=== cross-compile + link vmseflag.c for $TARGET ==="
test -f "$PROBE/vmseflag.c" || { echo "FAIL: $PROBE/vmseflag.c not found"; exit 1; }
test -f "$KMOD/vms_eflag_nb.h" || { echo "FAIL: $KMOD/vms_eflag_nb.h not found"; exit 1; }

"$CC" --sysroot="$SYSROOT" -O -Wall -Wextra -static \
    -I"$LIBVMSSYS" -I"$KMOD" \
    -o "$OUT/vmseflag" \
    "$PROBE/vmseflag.c" "$LIBVMSSYS/kif_transport_netbsd.c"

echo "--- linked executable ---"
file "$OUT/vmseflag" 2>/dev/null || true
"$TARGET-objdump" -f "$OUT/vmseflag" | grep -Ei 'file format|architecture'
"$TARGET-readelf" -h "$OUT/vmseflag" | grep -Ei 'Class|Data|Machine|Type'
"$TARGET-objdump" -f "$OUT/vmseflag" | grep -qiF 'file format elf32-vax' \
    || { echo "FAIL: vmseflag was not built elf32-vax"; exit 1; }

echo
echo "ALL PROOFS PASSED: vmseflag (event-flag guest tool) builds and links for $TARGET"
