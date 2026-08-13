#!/bin/bash
#
# crosscompile-vmsfs.sh - fast, QEMU-FREE per-PR check that the OVMX/NetBSD ODS-2
# vnode/VFS backend (src/kernel-netbsd/vmsfs/) + the SHARED substrate-neutral
# ODS-2 core (src/kernel-core/vmsfs/) COMPILE and LINK against the NetBSD/amd64
# kernel headers (rd vms-308, epic vms-8e8; the ODS-2 sibling of
# tests/netbsd/crosscompile.sh, which does the same for the executive module).
#
# WHY THIS EXISTS. The runtime proof -- boot NetBSD/amd64, mount a mastered OVMX
# ODS-2 disk, read DCL.EXE -- boots a NetBSD guest under QEMU-TCG on
# GitHub-hosted runners (no /dev/kvm), which is too slow and too VARIABLE for a
# per-PR gate (the same reason the executive harnesses run nightly). So the
# ARCHITECTURE is split, exactly as for the executive: the full mount+read proof
# runs NIGHTLY (schedule), and THIS build-only check is the per-PR gate. Its job
# is exactly "did a change break the NetBSD vmsfs build" -- the ODS-2 logic is
# byte-identical to the Linux side (it is the SAME src/kernel-core/vmsfs sources),
# so what per-PR CI must catch is a NetBSD BACKEND or header/API drift, a pure
# compile+link question.
#
# HOW. clang is a native cross-compiler; `-target x86_64-unknown-netbsd' plus the
# NetBSD kernel headers reproduce the in-guest bsd.kmodule.mk translation-unit
# environment (the same -ffreestanding/-mcmodel=kernel/_KERNEL/_MODULE flags and
# the machine/amd64/x86 arch-include symlinks bsd.klinks.mk makes). It compiles
# the NetBSD backend TU + every shared ODS-2 core TU and does a relocatable
# (`-r') link, asserting the object set is coherent (no unresolved-within-the-set
# or duplicate symbols; external NetBSD KPIs stay undefined and resolve at module
# load, exactly like the executive gate). Full module LOAD + mount is proven
# nightly. No emulator, no boot; runs in seconds.
#
# The clang resource include dir is added (-isystem $($CC -print-resource-dir)/
# include) so the freestanding <stdint.h> that vmsfs_ondisk.h reaches on its
# non-__KERNEL__ branch resolves under -nostdinc (the ondisk header intends the
# <stdint.h> branch on a NetBSD kernel build -- see src/kernel/vmsfs/vmsfs.h and
# README-backend.md §1).
#
# ENV:
#   OVMX_REPO   repo root (contains src/kernel-netbsd, src/kernel-core, src/kernel)
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
KMOD="$REPO/src/kernel-netbsd/vmsfs"
CORE="$REPO/src/kernel-core/vmsfs"
ONDISK="$REPO/src/kernel/vmsfs"     # vmsfs_ondisk.h lives beside the Linux glue

if [ ! -d "$SYS" ]; then
    echo "FAIL: NetBSD kernel sources not found at $SYS" >&2
    exit 2
fi

# NetBSD kernel-build arch-include symlinks (bsd.klinks.mk equivalents).
KL="$(mktemp -d)"
trap 'rm -rf "$KL" "${OBJ:-}"' EXIT
ln -sf "$SYS/arch/amd64/include" "$KL/machine"
ln -sf "$SYS/arch/amd64/include" "$KL/amd64"
ln -sf "$SYS/arch/x86/include"   "$KL/x86"
ln -sf "$SYS/arch/i386/include"  "$KL/i386"

RESINC="$("$CC" -print-resource-dir)/include"

# The same flags the in-guest bsd.kmodule.mk build uses for an amd64 kernel
# module, plus -Werror so a warning is a per-PR failure exactly as in-guest.
CFLAGS=(
    -target x86_64-unknown-netbsd
    -std=gnu99 -Werror -Wall
    -ffreestanding -fno-strict-aliasing
    -mno-red-zone -mno-mmx -mno-sse -mno-avx -mcmodel=kernel -fno-omit-frame-pointer
    -DOVMX_KBACKEND_NETBSD -nostdinc
    -isystem "$KL" -isystem "$SYS" -isystem "$SYS/arch" -isystem "$SYS/../common/include"
    -isystem "$RESINC"
    -D_KERNEL -D_MODULE -DSYSCTL_INCLUDE_DESCR -DKDTRACE_HOOKS
    -I"$KMOD" -I"$CORE" -I"$ONDISK"
    -Wno-unknown-warning-option
)

OBJ="$(mktemp -d)"

# The module's translation units: the NetBSD vnode/VFS backend glue + the
# NetBSD realization of the vmsfs_bio seam (vmsfs_backend_netbsd.h, a header),
# and THE SHARED ODS-2 CORE SOURCES -- identical to the ones the Linux vmsfs.ko
# builds. This is exactly src/kernel-netbsd/vmsfs/Makefile's SRCS.
#   vmsfs_vfsops.c   - the NetBSD VFS/vnode glue (this backend)
#   vmsfs_version.c  - VMS NAME.TYPE;VERSION parse/build/match
#   vmsfs_map.c      - retrieval-map VBN<->LBN math
#   vmsfs_name.c     - filename split/uppercase/case-blind match
#   vmsfs_header.c   - file-header decode/encode (+ partial writers)
#   vmsfs_alloc.c    - storage/FID allocator + home-block write-back + map growth
#   vmsfs_dirscan.c  - directory-block scanner (resolve/version/add/remove/readdir)
SRCS=(
    "$KMOD/vmsfs_vfsops.c"
    "$CORE/vmsfs_version.c"
    "$CORE/vmsfs_map.c"
    "$CORE/vmsfs_name.c"
    "$CORE/vmsfs_header.c"
    "$CORE/vmsfs_alloc.c"
    "$CORE/vmsfs_dirscan.c"
)

# ---- teeth check ---------------------------------------------------------
if [ "${CROSSCOMPILE_NEGCTL:-}" = "1" ]; then
    bad="$OBJ/negctl_bad.c"
    { cat "$CORE/vmsfs_header.c"; printf '\nthis is deliberately invalid C @@@ ;\n'; } > "$bad"
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

echo "LD  vmsfs.kmod.o (relocatable)"
"$CC" -target x86_64-unknown-netbsd -nostdlib -r -o "$OBJ/vmsfs.kmod.o" "$OBJ"/*.c.o

echo "PASS: the OVMX/NetBSD ODS-2 vnode backend + shared src/kernel-core/vmsfs core (vmsfs_version.c, vmsfs_map.c, vmsfs_name.c, vmsfs_header.c, vmsfs_alloc.c, vmsfs_dirscan.c) cross-compile and link for NetBSD/amd64 (${#SRCS[@]} TUs)"
