#!/bin/bash
#
# crosscompile.sh - fast, QEMU-FREE per-PR check that the OVMX/NetBSD `vms'
# kernel module + the shared src/kernel-core/ facility COMPILE and LINK against
# the NetBSD/amd64 kernel headers (rd vms-2d9, epic vms-8e8).
#
# WHY THIS EXISTS. The console-driven NetBSD harnesses (P2a smoke, P2b
# pseudo-device, P2c event-flag) prove the executive RUNS on NetBSD, but they
# boot a NetBSD guest under QEMU-TCG on GitHub-hosted runners (no /dev/kvm),
# which is too slow and too VARIABLE for a reliable per-PR gate -- they pass 5/5
# locally yet flake intermittently on CI. So the ARCHITECTURE is split: the full
# runtime proof runs NIGHTLY (schedule), and THIS build-only check is the per-PR
# gate. Its job is exactly "did a change break the NetBSD build" -- the facility
# logic is byte-identical Linux-side, so what per-PR CI must catch is a NetBSD
# BACKEND or header/API drift, and that is a pure compile+link question.
#
# HOW. clang is a native cross-compiler; `-target x86_64-unknown-netbsd' plus the
# NetBSD kernel headers (from the pinned syssrc set) reproduce the same
# translation-unit environment as the in-guest bsd.kmodule.mk build -- the same
# -ffreestanding/-mcmodel=kernel/_KERNEL/_MODULE flags and the same
# machine/amd64/x86 arch-include symlinks the kernel build makes (bsd.klinks.mk).
# No emulator, no boot; runs in seconds. It compiles every module translation
# unit and does a relocatable (`-r') link, asserting the object set is coherent
# (no unresolved-within-the-set or duplicate symbols). Full module LOAD is a
# runtime property proven nightly.
#
# ENV:
#   OVMX_REPO   repo root (contains src/kernel-netbsd, src/kernel-core)
#   NBSRC       extracted NetBSD source root (contains usr/src/sys ...)
#   CC          the cross compiler (default: clang)
#   CROSSCOMPILE_NEGCTL=1   teeth check: compile a deliberately-broken TU and
#                           assert the build FAILS (so a real break can't slip by)
#
# Clean-room (CLAUDE.md Rule 8): OVMX's own build glue over the PUBLIC NetBSD
# kernel headers + a stock clang. No NetBSD or VSI source is copied into OVMX.

set -euo pipefail

REPO="${OVMX_REPO:?set OVMX_REPO to the repo root}"
NBSRC="${NBSRC:?set NBSRC to the extracted NetBSD source root}"
CC="${CC:-clang}"

SYS="$NBSRC/usr/src/sys"
KMOD="$REPO/src/kernel-netbsd"
CORE="$REPO/src/kernel-core"

if [ ! -d "$SYS" ]; then
    echo "FAIL: NetBSD kernel sources not found at $SYS" >&2
    exit 2
fi

# NetBSD kernel-build arch-include symlinks (what bsd.klinks.mk creates in the
# build dir): `#include <machine/...>' / <amd64/...> / <x86/...> resolve here.
KL="$(mktemp -d)"
trap 'rm -rf "$KL" "${OBJ:-}"' EXIT
ln -sf "$SYS/arch/amd64/include" "$KL/machine"
ln -sf "$SYS/arch/amd64/include" "$KL/amd64"
ln -sf "$SYS/arch/x86/include"   "$KL/x86"
ln -sf "$SYS/arch/i386/include"  "$KL/i386"

# The same flags the in-guest bsd.kmodule.mk build uses for an amd64 kernel
# module, plus -Werror so a warning is a per-PR failure exactly as in-guest.
CFLAGS=(
    -target x86_64-unknown-netbsd
    -std=gnu99 -Werror -Wall
    -ffreestanding -fno-strict-aliasing
    -mno-red-zone -mno-mmx -mno-sse -mno-avx -mcmodel=kernel -fno-omit-frame-pointer
    -DOVMX_KBACKEND_NETBSD -nostdinc
    -isystem "$KL" -isystem "$SYS" -isystem "$SYS/arch" -isystem "$SYS/../common/include"
    -D_KERNEL -D_MODULE -DSYSCTL_INCLUDE_DESCR -DKDTRACE_HOOKS
    -I"$KMOD" -I"$CORE"
    -Wno-unknown-warning-option
)

OBJ="$(mktemp -d)"

# The module's translation units: the NetBSD backend glue, the OVMX intrusive
# list, and THE SHARED facility sources (identical to the ones the Linux vms.ko
# builds). This is exactly src/kernel-netbsd/Makefile's SRCS.
#   vms_eflag.c / vms_ast.c / vms_access.c - event flags, ASTs, access modes
#   vms_mbx.c   - executive-resident mailboxes MBAn: (rd vms-d7a); links
#                 vms_ast_notify_arrival, so vms_ast.c must be listed too
SRCS=(
    "$KMOD/vms_netbsd.c"
    "$KMOD/exec_list_netbsd.c"
    "$CORE/vms_eflag.c"
    "$CORE/vms_ast.c"
    "$CORE/vms_access.c"
    "$CORE/vms_mbx.c"
)

# ---- teeth check ---------------------------------------------------------
if [ "${CROSSCOMPILE_NEGCTL:-}" = "1" ]; then
    bad="$OBJ/negctl_bad.c"
    { cat "$CORE/vms_eflag.c"; printf '\nthis is deliberately invalid C @@@ ;\n'; } > "$bad"
    if "$CC" "${CFLAGS[@]}" -c "$bad" -o /dev/null 2>/dev/null; then
        echo "FAIL (negctl): a deliberately-broken NetBSD TU COMPILED -- the cross-compile check has NO TEETH"
        exit 1
    fi
    echo "PASS (negctl): a deliberately-broken NetBSD TU fails the cross-compile, as it must"
    exit 0
fi

# ---- the real check ------------------------------------------------------
echo "clang: $($CC --version | head -1)"
for s in "${SRCS[@]}"; do
    echo "CC  $(basename "$s")"
    "$CC" "${CFLAGS[@]}" -c "$s" -o "$OBJ/$(basename "$s").o"
done

echo "LD  vms.kmod.o (relocatable)"
"$CC" -target x86_64-unknown-netbsd -nostdlib -r -o "$OBJ/vms.kmod.o" "$OBJ"/*.c.o

echo "PASS: the OVMX/NetBSD vms module + shared src/kernel-core facilities (vms_eflag.c, vms_ast.c, vms_access.c) cross-compile and link for NetBSD/amd64 (${#SRCS[@]} TUs)"
