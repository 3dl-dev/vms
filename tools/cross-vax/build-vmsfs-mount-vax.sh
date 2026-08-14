#!/bin/sh
# build-vmsfs-mount-vax.sh - produce the two elf32-vax artifacts the vms-544d
# runtime proof (OVMX ODS-2 volume MOUNTS + READS on NetBSD/vax under SIMH; epic
# vms-8e8) loads:
#
#   1. vmsfs.kmod  - the OVMX/NetBSD ODS-2 vnode/VFS LOADABLE kernel module for
#                    NetBSD/vax: vmsfs_vfsops.c (the NetBSD VFS glue,
#                    MODULE_CLASS_VFS -> vfs_attach) + the SAME substrate-neutral
#                    ODS-2 core (src/kernel-core/vmsfs/*.c) the Linux vmsfs.ko and
#                    the NetBSD/amd64 mount proof (vms-308) build. ONE ODS-2 core,
#                    thin per-substrate backend (INV-DRIFT); this cross-compiles
#                    it with the NetBSD backend for the 32-bit VAX.
#   2. vmsfs_mount - the tiny userspace mount(2) helper (tests/netbsd/guest/
#                    vmsfs_mount.c) that mounts a "vmsfs" volume read-only,
#                    statically linked so the guest needs no shared-lib plumbing.
#
# This is the ODS-2 sibling of tools/cross-vax/build-devvms-vax.sh (the executive
# vms.kmod + vmsprobe builder). The SAME two VAX-specific constraints apply, for
# the SAME reasons, and are proven for the executive module already:
#   * -O2  : the shared core uses C99 `inline' helpers (e.g. the retrieval-map
#            math); at -O0 they become undefined externals and the module cannot
#            load. bsd.kmodule.mk builds the kernel at -O2.
#   * -fno-pic : else the object carries R_VAX_GOT32 relocations the kernel's
#            GOT-less kobj loader rejects ("Bad relocation ... type=7").
# We ASSERT the linked module has NO unresolved OVMX symbol -- only real NetBSD
# KPIs (bread/brelse/vfs_attach/kmem_alloc/...) bound at modload.
#
# Runs INSIDE the ovmx-cross-vax container. The pinned NetBSD/vax kernel headers
# (syssrc) are mounted read-only at $NBSRC. Nothing here touches the host.
#
# Clean-room (CLAUDE.md Rule 8): OVMX's own build glue over PUBLIC NetBSD kernel
# headers + a stock gcc. No NetBSD or VSI/HPE source is copied.
#
# ENV: NBSRC (default /nbsrc), OUT (default /out). Exit 0 = both artifacts built.
set -eu

TARGET="${TARGET:-vax--netbsdelf}"
CC="${TARGET}-gcc"
NM="${TARGET}-nm"
OBJDUMP="${TARGET}-objdump"
SRC="$(pwd)"
KMOD="$SRC/src/kernel-netbsd/vmsfs"     # NetBSD VFS glue + backend headers
CORE="$SRC/src/kernel-core/vmsfs"       # shared ODS-2 core
ONDISK_SRC="$SRC/src/kernel/vmsfs"      # vmsfs_ondisk.h lives beside the Linux glue
GUEST="$SRC/tests/netbsd/guest"         # vmsfs_mount.c
NBSRC="${NBSRC:-/nbsrc}"
SYS="$NBSRC/usr/src/sys"
OUT="${OUT:-/out}"
mkdir -p "$OUT"; rm -f "$OUT"/vmsfs.kmod "$OUT"/vmsfs_mount "$OUT"/*.o 2>/dev/null || true

[ -d "$SYS" ] || { echo "FAIL: NetBSD kernel sources not at $SYS (mount syssrc at \$NBSRC)" >&2; exit 2; }

# bsd.klinks.mk arch-include symlinks: <machine/...> / <vax/...> resolve here.
KL="$(mktemp -d)"
# Stage ONLY vmsfs_ondisk.h as the `../ondisk' include dir the Makefile documents
# (do not -I the whole Linux glue dir -- it would expose vmsfs_backend_linux.h).
OND="$(mktemp -d)"
trap 'rm -rf "$KL" "$OND"' EXIT
ln -sf "$SYS/arch/vax/include" "$KL/machine"
ln -sf "$SYS/arch/vax/include" "$KL/vax"
cp "$ONDISK_SRC/vmsfs_ondisk.h" "$OND/"

# The gcc freestanding include dir (stdint.h/stddef.h/stdarg.h): -nostdinc hides
# it, but vmsfs_ondisk.h includes <stdint.h> (it is shared with the userspace
# master tool), so re-add it exactly as a NetBSD kernel build does.
GCCINC="$("$CC" -print-file-name=include)"
CFLAGS="-std=gnu99 -O2 -fno-pic -Werror -Wall -ffreestanding -fno-strict-aliasing -fno-omit-frame-pointer"
CPPFLAGS="-DOVMX_KBACKEND_NETBSD -nostdinc -isystem $GCCINC -isystem $KL -isystem $SYS -isystem $SYS/arch -isystem $SYS/../common/include -D_KERNEL -D_MODULE -I$KMOD -I$CORE -I$OND ${EXTRA_CPPFLAGS:-}"

# EXACTLY src/kernel-netbsd/vmsfs/Makefile's SRCS: the NetBSD VFS glue + the
# shared ODS-2 core (one implementation of every ODS-2 decision).
SRCS="$KMOD/vmsfs_vfsops.c \
      $CORE/vmsfs_version.c \
      $CORE/vmsfs_map.c \
      $CORE/vmsfs_name.c \
      $CORE/vmsfs_header.c \
      $CORE/vmsfs_alloc.c \
      $CORE/vmsfs_dirscan.c"

# ---- teeth check ---------------------------------------------------------
# A deliberately-broken TU MUST fail the cross-compile, or a real break slips by
# (mirrors build-vms-module-vax.sh's CROSSCOMPILE_NEGCTL).
if [ "${CROSSCOMPILE_NEGCTL:-}" = "1" ]; then
    bad="$OUT/negctl_bad.c"
    { cat "$CORE/vmsfs_version.c"; printf '\nthis is deliberately invalid C @@@ ;\n'; } > "$bad"
    # shellcheck disable=SC2086
    if "$CC" $CFLAGS $CPPFLAGS -c "$bad" -o /dev/null 2>/dev/null; then
        echo "FAIL (negctl): a deliberately-broken VAX vmsfs TU COMPILED -- NO TEETH"; exit 1
    fi
    echo "PASS (negctl): a deliberately-broken vmsfs TU fails the elf32-vax cross-compile"; exit 0
fi

echo "=== toolchain ==="; "$CC" --version | head -1; "$CC" -dumpmachine; echo

echo "=== compile each vmsfs module TU at -O2 -fno-pic for elf32-vax ==="
OBJS=""
for s in $SRCS; do
    b="$(basename "$s")"; o="$OUT/${b%.c}.o"
    echo "--- $CC -c $b ---"
    # shellcheck disable=SC2086
    "$CC" $CFLAGS $CPPFLAGS -c "$s" -o "$o"
    OBJS="$OBJS $o"
done
echo

echo "=== relocatable link the loadable vmsfs module (vmsfs.kmod) ==="
# shellcheck disable=SC2086
"$CC" -nostdlib -r -o "$OUT/vmsfs.kmod" $OBJS
echo "  linked $OUT/vmsfs.kmod"; echo

echo "=== assert NO OVMX symbol is left undefined (only real kernel KPIs may be) ==="
BAD="$("$NM" "$OUT/vmsfs.kmod" | awk '$1=="U"{print $2}' | grep -E '^(vmsfs_|exec_)' || true)"
if [ -n "$BAD" ]; then
    echo "FAIL: vmsfs.kmod has UNRESOLVED OVMX symbols -- it could not modload:" >&2
    echo "$BAD" | sed 's/^/    /' >&2
    exit 1
fi
echo "  OK: every vmsfs_*/exec_* symbol resolved; residual undefined are NetBSD KPIs:"
"$NM" "$OUT/vmsfs.kmod" | awk '$1=="U"{print "    "$2}' | sort
echo

echo "=== assert elf32-vax + module metadata (link_set_modules) ==="
# Non-quiet grep: emit the "... file format elf32-vax" line so the target is
# visible in the log (and the CI cross gate can key on it) as well as asserted.
"$OBJDUMP" -f "$OUT/vmsfs.kmod" | grep -iF 'file format elf32-vax' || { echo "FAIL: not elf32-vax"; exit 1; }
"$OBJDUMP" -h "$OUT/vmsfs.kmod" | grep -q 'link_set_modules' || { echo "FAIL: no link_set_modules (MODULE() metadata missing)"; exit 1; }
"$NM" "$OUT/vmsfs.kmod" | grep -q 'vmsfs_modinfo' || { echo "FAIL: no vmsfs_modinfo"; exit 1; }
# no GOT32 relocations may remain (kobj-unloadable)
if "$OBJDUMP" -r "$OUT/vmsfs.kmod" 2>/dev/null | grep -qi 'R_VAX_GOT32'; then
    echo "FAIL: vmsfs.kmod carries R_VAX_GOT32 relocations (rebuild -fno-pic)"; exit 1; fi
echo "  OK: elf32-vax loadable VFS module with MODULE() metadata, no GOT32 relocs"; echo

echo "=== build the userspace mount helper (static elf32-vax) ==="
"$CC" -O -Wall -Wextra -static -o "$OUT/vmsfs_mount" "$GUEST/vmsfs_mount.c"
"$OBJDUMP" -f "$OUT/vmsfs_mount" | grep -iF 'file format elf32-vax' || { echo "FAIL: vmsfs_mount not elf32-vax"; exit 1; }
echo "  OK: vmsfs_mount (static elf32-vax)"; echo

echo "=== ARTIFACTS ==="; ls -l "$OUT/vmsfs.kmod" "$OUT/vmsfs_mount"
echo "=== build-vmsfs-mount-vax.sh: DONE (loadable vmsfs.kmod + mount helper for SIMH) ==="
