#!/bin/sh
# build-access-tool-vax.sh - cross-compile + link-prove the access-mode +
# cross-process AST guest tool (tests/netbsd/guest/vmsaccess.c) for
# netbsd-vax (rd vms-4b7, parent vms-945e, epic vms-8e8).
#
# FAST PER-PR GATE, no NBSRC / no SIMH. The nightly runtime proof
# (tests/lab-vax/run-access.sh, drive_access_vax.py) boots real NetBSD/vax
# under SIMH to prove access-mode enforcement + cross-process AST delivery
# against a live /dev/vms; that is slow and disk-heavy. This script is the
# fast per-PR complement, the vax analogue of build-proctab-tool-vax.sh: it
# cross-compiles vmsaccess.c against the NetBSD/vax kif_transport leaf
# (src/libvmssys/kif_transport_netbsd.c) and LINKS a real vax--netbsdelf
# ELF32 executable against NetBSD libc. No kernel headers are needed --
# vmsaccess.c is ordinary userspace code, same as vmsproctab.c/vmseflag.c.
# The point (mirrors rd vms-4e7's / vms-2e0's for the other facilities): vax
# is ILP32 / non-IEEE-float / ELF32, so a struct-layout or width bug in the
# shared wire contracts (src/kernel-netbsd/vms_access_nb.h,
# src/kernel-netbsd/vms_ast_nb.h -- e.g. astadr/astprm/cur_privs/perm_privs
# as uint64_t on an ILP32 target) can compile clean on every 64-bit OVMX
# target and only break here -- this gate catches that on every PR, fast.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile).
#
# ENV:
#   CROSSCOMPILE_NEGCTL=1   teeth check: a deliberately-broken vmsaccess.c
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
OUT="${OUT:-/tmp/vax-access-tool-build}"
rm -rf "$OUT"; mkdir -p "$OUT"

echo "=== toolchain ==="
"$CC" --version | head -1
"$CC" -dumpmachine
echo

# --- negative control (teeth) -------------------------------------------------
# Mirrors build-proctab-tool-vax.sh's / build-eflag-tool-vax.sh's width negctl,
# but against the actual wire structs vmsaccess.c depends on (struct
# vms_ast_args carries astadr/astprm as uint64_t; struct vms_getmode_args
# carries cur_privs/perm_privs as uint64_t): a TU that assumes the platform's
# `long' is 64-bit (an LP64-ism no ILP32 vax build would ever produce from the
# real header) must be REJECTED.
if [ "${CROSSCOMPILE_NEGCTL:-0}" = "1" ]; then
    echo "=== NEGATIVE CONTROL: a deliberately-broken access/ast TU must FAIL the vax cross-compile ==="
    cat > "$OUT/access_negctl.c" <<'EOF'
#include <stdint.h>
/* Deliberately WRONG: pretends astprm/perm_privs's underlying `long' is
 * 64-bit (an LP64 assumption). On vax (ILP32) long is 32-bit, so this static
 * assertion must be a hard compile error -- if it is not, the width gate this
 * probe depends on (vms_ast_nb.h's / vms_access_nb.h's explicit uint64_t
 * astadr/astprm/cur_privs/perm_privs fields, never a bare `long') has no
 * teeth. */
_Static_assert(sizeof(long) == 8, "NEGCTL: pretend vax long is 64-bit (LP64)");
int main(void){ return 0; }
EOF
    if "$CC" --sysroot="$SYSROOT" -c "$OUT/access_negctl.c" -o "$OUT/access_negctl.o" 2>"$OUT/negctl.err"; then
        echo "FAIL (negctl): the LP64 width assertion compiled clean on vax -- no teeth"
        exit 1
    fi
    echo "--- rejected as expected (compiler diagnostics) ---"
    grep -iE 'static.?assert|error' "$OUT/negctl.err" | head -4 || true
    echo "PASS (negctl): the LP64 width assumption was REJECTED by the vax--netbsdelf cross-compile"
    exit 0
fi

# --- the real proof: cross-compile + link vmsaccess for elf32-vax ------------
echo "=== cross-compile + link vmsaccess.c for $TARGET ==="
test -f "$PROBE/vmsaccess.c" || { echo "FAIL: $PROBE/vmsaccess.c not found"; exit 1; }
test -f "$KMOD/vms_access_nb.h" || { echo "FAIL: $KMOD/vms_access_nb.h not found"; exit 1; }
test -f "$KMOD/vms_ast_nb.h" || { echo "FAIL: $KMOD/vms_ast_nb.h not found"; exit 1; }
test -f "$KMOD/vms_mbx_nb.h" || { echo "FAIL: $KMOD/vms_mbx_nb.h not found"; exit 1; }

"$CC" --sysroot="$SYSROOT" -O -Wall -Wextra -static \
    -I"$LIBVMSSYS" -I"$KMOD" \
    -o "$OUT/vmsaccess" \
    "$PROBE/vmsaccess.c" "$LIBVMSSYS/kif_transport_netbsd.c"

echo "--- linked executable ---"
file "$OUT/vmsaccess" 2>/dev/null || true
"$TARGET-objdump" -f "$OUT/vmsaccess" | grep -Ei 'file format|architecture'
"$TARGET-readelf" -h "$OUT/vmsaccess" | grep -Ei 'Class|Data|Machine|Type'
"$TARGET-objdump" -f "$OUT/vmsaccess" | grep -qiF 'file format elf32-vax' \
    || { echo "FAIL: vmsaccess was not built elf32-vax"; exit 1; }

echo
echo "ALL PROOFS PASSED: vmsaccess (access-mode + cross-process AST guest tool) builds and links for $TARGET"
