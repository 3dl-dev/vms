#!/bin/sh
# build-devvms-vax.sh - produce the two elf32-vax artifacts the P4-B runtime
# proof (rd vms-f78bb, epic vms-8e8) loads on real NetBSD/vax under SIMH:
#
#   1. vms.kmod  - the OVMX executive `vms' LOADABLE kernel module for
#                  NetBSD/vax: the SAME 10 translation units B1's per-PR
#                  cross-compile gate (tools/cross-vax/build-vms-module-vax.sh,
#                  rd vms-20b9) proves width-clean, but built here as a
#                  genuinely LOAD-ABLE module -- at -O2 with every OVMX symbol
#                  RESOLVED (not merely `-r' compile-coherent), so `modload'
#                  binds it against a running kernel with only real NetBSD KPIs
#                  left undefined.
#   2. vmsprobe  - the userspace ping probe (tests/netbsd/guest/vmsprobe.c)
#                  reaching /dev/vms THROUGH the NetBSD transport seam
#                  (src/libvmssys/kif_transport_netbsd.c), statically linked so
#                  it needs no shared-library plumbing in the guest.
#
# WHY -O2 AND NOT B1's FLAGS. B1 is a width-audit GATE: it compiles at the
# default -O0 and does a relocatable (`-r') link, which permits UNRESOLVED
# symbols -- fine for proving "the TUs compile ILP32-clean and have no duplicate
# defs", but a `-r' link does not care that exec_jhash/exec_hash_bucket_of (the
# lock manager's resource-hash helpers, src/kernel-netbsd/exec_hash_netbsd.c)
# are reachable. A module that modload'd with those unresolved would fail to
# load. So this producer compiles at -O2 (as an in-guest bsd.kmodule.mk build
# would) and then ASSERTS that the only undefined symbols left are real kernel
# KPIs -- no `exec_*'/`vms_*' OVMX symbol may be undefined.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile). The
# pinned NetBSD/vax kernel headers (syssrc) are mounted read-only at $NBSRC.
# Nothing here touches the host (Rule 9 / containerize-all-deps).
#
# Clean-room (CLAUDE.md Rule 8): OVMX's own build glue over the PUBLIC NetBSD
# kernel headers + a stock gcc. No NetBSD or VSI/HPE source is copied.
#
# ENV:
#   NBSRC   extracted NetBSD syssrc root (contains usr/src/sys ...). Default /nbsrc.
#   OUT     output dir for vms.kmod + vmsprobe. Default /out.
#
# Exit 0 = both artifacts built, module has every OVMX symbol resolved.

set -eu

TARGET="${TARGET:-vax--netbsdelf}"
CC="${TARGET}-gcc"
NM="${TARGET}-nm"
OBJDUMP="${TARGET}-objdump"
SRC="$(pwd)"
KMOD="$SRC/src/kernel-netbsd"
CORE="$SRC/src/kernel-core"
LIBVMSSYS="$SRC/src/libvmssys"
PROBE="$SRC/tests/netbsd/guest"
NBSRC="${NBSRC:-/nbsrc}"
SYS="$NBSRC/usr/src/sys"
OUT="${OUT:-/out}"
# Remove ONLY this script's own outputs -- NOT the whole dir: run-devvms.sh
# shares OUT (devvms-artifacts) with the cached custom kernel (netbsd-OVMX), and
# wiping the dir here would delete that cache and force a ~40-min build.sh rerun.
mkdir -p "$OUT"; rm -f "$OUT"/vms.kmod "$OUT"/vmsprobe "$OUT"/*.o 2>/dev/null || true

[ -d "$SYS" ] || { echo "FAIL: NetBSD kernel sources not at $SYS (mount syssrc at \$NBSRC)" >&2; exit 2; }

# bsd.klinks.mk arch-include symlinks: <machine/...> / <vax/...> resolve here.
KL="$(mktemp -d)"
trap 'rm -rf "$KL"' EXIT
ln -sf "$SYS/arch/vax/include" "$KL/machine"
ln -sf "$SYS/arch/vax/include" "$KL/vax"

# The same freestanding kernel-module TU environment an in-guest bsd.kmodule.mk
# VAX build uses -- at -O2 (kernel default) so the resource-hash inline uses
# link, and -fno-pic so the object carries ABSOLUTE (R_VAX_32) relocations, NOT
# GOT-relative (R_VAX_GOT32, reloc type 7) ones. This is REQUIRED to modload on
# NetBSD/vax: the kernel is built -fno-pic and has no GOT, so its kobj loader
# rejects a GOT32 relocation ("Bad relocation ... type=7 ... unresolved rela").
# bsd.kmodule.mk adds -fno-pic for exactly this reason (share/mk/bsd.kmodule.mk),
# and the vax kernel Makefile builds -fno-pic. -Werror so a warning is fatal.
CFLAGS="-std=gnu99 -O2 -fno-pic -Werror -Wall -ffreestanding -fno-strict-aliasing -fno-omit-frame-pointer"
CPPFLAGS="-DOVMX_KBACKEND_NETBSD -nostdinc -isystem $KL -isystem $SYS -isystem $SYS/arch -isystem $SYS/../common/include -D_KERNEL -D_MODULE -I$KMOD -I$CORE"

# EXACTLY src/kernel-netbsd/Makefile's SRCS (= B1's SRCS): the NetBSD backend
# glue + OVMX intrusive containers + the SHARED executive facility sources.
SRCS="$KMOD/vms_netbsd.c \
      $KMOD/vms_lnm_arena_netbsd.c \
      $KMOD/exec_list_netbsd.c \
      $KMOD/exec_hash_netbsd.c \
      $KMOD/exec_rbtree_netbsd.c \
      $CORE/vms_eflag.c \
      $CORE/vms_ast.c \
      $CORE/vms_access.c \
      $CORE/vms_mbx.c \
      $CORE/vms_proctab.c \
      $CORE/vms_lock.c \
      $CORE/vms_lnm.c"

echo "=== toolchain ==="; "$CC" --version | head -1; "$CC" -dumpmachine; echo

echo "=== compile each module TU at -O2 for elf32-vax ==="
OBJS=""
for s in $SRCS; do
    b="$(basename "$s")"; o="$OUT/${b%.c}.o"
    echo "--- $CC -O2 -c $b ---"
    # shellcheck disable=SC2086
    "$CC" $CFLAGS $CPPFLAGS -c "$s" -o "$o"
    OBJS="$OBJS $o"
done
echo

echo "=== relocatable link the loadable module (vms.kmod) ==="
# shellcheck disable=SC2086
"$CC" -nostdlib -r -o "$OUT/vms.kmod" $OBJS
echo "  linked $OUT/vms.kmod"
echo

echo "=== assert NO OVMX symbol is left undefined (only real kernel KPIs may be) ==="
BAD="$("$NM" "$OUT/vms.kmod" | awk '$1=="U"{print $2}' | grep -E '^(exec_|vms_)' || true)"
if [ -n "$BAD" ]; then
    echo "FAIL: the module has UNRESOLVED OVMX symbols -- it could not modload:" >&2
    echo "$BAD" | sed 's/^/    /' >&2
    exit 1
fi
echo "  OK: every exec_*/vms_* symbol is resolved; residual undefined are NetBSD KPIs:"
"$NM" "$OUT/vms.kmod" | awk '$1=="U"{print "    "$2}' | sort
echo

echo "=== assert module is elf32-vax + carries module metadata (link_set_modules) ==="
"$OBJDUMP" -f "$OUT/vms.kmod" | grep -qiF 'file format elf32-vax' || { echo "FAIL: vms.kmod not elf32-vax"; exit 1; }
"$OBJDUMP" -h "$OUT/vms.kmod" | grep -q 'link_set_modules' || { echo "FAIL: vms.kmod has no link_set_modules (MODULE() metadata missing)"; exit 1; }
"$NM" "$OUT/vms.kmod" | grep -q 'vms_modinfo' || { echo "FAIL: vms.kmod has no vms_modinfo"; exit 1; }
echo "  OK: elf32-vax loadable module with MODULE() metadata"
echo

echo "=== build the userspace ping probe (static elf32-vax) ==="
# Static so the guest needs no ld.elf_so / shared-lib staging to run it.
"$CC" -O -Wall -Wextra -static \
    -I"$LIBVMSSYS" -I"$KMOD" \
    -o "$OUT/vmsprobe" \
    "$PROBE/vmsprobe.c" "$LIBVMSSYS/kif_transport_netbsd.c"
"$OBJDUMP" -f "$OUT/vmsprobe" | grep -qiF 'file format elf32-vax' || { echo "FAIL: vmsprobe not elf32-vax"; exit 1; }
echo "  OK: vmsprobe (static elf32-vax)"
echo

echo "=== ARTIFACTS ==="
ls -l "$OUT/vms.kmod" "$OUT/vmsprobe"
echo "=== build-devvms-vax.sh: DONE (both elf32-vax artifacts ready for SIMH) ==="
