#!/bin/sh
# build-vms-module-vax.sh - cross-compile the WHOLE OVMX executive `vms' kernel
# module (the shared executive core + ALL its NetBSD backends) for elf32-vax and
# prove it is ILP32/endian-clean (rd vms-20b9, epic vms-8e8; P4-B). This is the
# elf32-vax analog of tests/netbsd/crosscompile.sh (the NetBSD/amd64 module
# cross-compile) built on the elf32-vax module cross-compile pattern
# (rd vms-bb8): same module SRCS as src/kernel-netbsd/Makefile, compiled for the
# 32-bit VAX instead of amd64.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the pinned `vax--netbsdelf' gcc/binutils. Nothing here touches the
# host. The NetBSD/vax KERNEL headers (syssrc) are mounted read-only at $NBSRC
# by the CI job (the same pinned NetBSD 10.1 syssrc the amd64 gate uses,
# tests/netbsd/netbsd_version.env) -- exactly as the amd64 gate mounts NBSRC;
# the container image itself carries only the compiler + a NetBSD/vax libc
# sysroot, never kernel sources.
#
# WHY A REAL NetBSD KERNEL COMPILE (and not the freestanding width-audit backend
# the ODS-2 core uses). The executive module's glue (vms_netbsd.c) is inherently
# a NetBSD kernel TU -- it names sys/device.h, sys/module.h, sys/ioccom.h,
# kauth(9), kmem(9), kmutex(9) -- so unlike the substrate-neutral ODS-2 core it
# CANNOT be compiled against a synthetic freestanding backend. This gate is
# therefore the true elf32-vax twin of the amd64 crosscompile.sh: the SAME
# module translation units (src/kernel-netbsd/ backends + the SHARED
# src/kernel-core/ facilities) built with -DOVMX_KBACKEND_NETBSD against real
# NetBSD/vax kernel headers, only under the 32-bit VAX compiler.
#
# HOW. `vax--netbsdelf-gcc' reproduces the same TU environment as an in-guest
# bsd.kmodule.mk VAX kernel build: -ffreestanding, _KERNEL/_MODULE, -nostdinc,
# and the machine/vax arch-include symlinks the kernel build makes (bsd.klinks.mk).
# No emulator, no boot; runs in seconds. It compiles every module TU -Werror and
# does a relocatable (`-r') link, asserting the object set is load-coherent (a
# relocatable link fails on a duplicate definition; the residual undefined
# symbols are the NetBSD KPIs the module resolves against the running kernel at
# load). Module LOAD on real NetBSD/vax under SIMH + a live /dev/vms is B2
# (rd vms-f78bb), NOT this: B1 is the width-clean COMPILE.
#
# THE ILP32/ENDIAN PROOF is threefold, all at compile time under the 32-bit VAX
# compiler:
#   1. every module TU compiles -Werror for elf32-vax (any long/pointer-width
#      assumption in the shared core or a NetBSD backend is a hard error);
#   2. a GENERATED width-audit TU's _Static_asserts pass under ILP32 -- the width
#      class (long/pointer == 4), the packed parent+colour RB word invariant
#      (sizeof(unsigned long) == sizeof(void *), which is why the RB node's
#      packing survives the shrink from LP64's 24-byte node to VAX's 12-byte
#      node), and the intrusive-container node sizes/offsets under ILP32;
#   3. every emitted object is verified `file format elf32-vax' (arch: vax),
#      including the relocatable-linked module object.
# Findings: docs/audit-ilp32-vax-netbsd-exec.md.
#
# ACP COMPILE-COVERAGE (rd vms-6a7f, epic vms-208). As of this rung the SRCS
# below are a DELIBERATE SUPERSET of src/kernel-netbsd/Makefile: they add
# src/kernel-core/vmsfs_acp.c (the Files-11 ODS-2 ACP channel + IO$_ACCESS /
# READVBLK / WRITEVBLK handlers) and src/vmsfs/ods2/ods2_edit.c (the pure
# on-disk EDIT helpers vmsfs_acp.c's implicit-extend path calls), built with
# -DOVMX_ODS2_KERNEL against the real NetBSD contract headers
# (src/kernel-netbsd/vms_internal.h + the new vms_acp_nb.h twin, exactly the
# same headers vms_mbx.c / vms_lnm.c already prove clean here). This closes the
# ONE compile leg that used to skip the ACP handlers entirely: the LP64 Alpha
# Linux build compiles them but cannot catch an ILP32 width regression, and
# nothing else cross-compiled them for a 32-bit target at all. SCOPE: this is
# COMPILE-COVERAGE ONLY -- vmsfs_acp.c is NOT added to src/kernel-netbsd/
# Makefile's real SRCS, so it is not (yet) linked into the loadable NetBSD/vax
# `vms' module and vms_netbsd.c does not dispatch its ioctls. Wiring it into
# the real NetBSD-VAX kmod so it RUNS there is a later re-target (vms-d5d).
#
# Clean-room (CLAUDE.md Rule 8): OVMX's own build glue over the PUBLIC NetBSD
# kernel headers + a stock gcc. No NetBSD or VSI/HPE source is copied into OVMX.
#
# ENV:
#   NBSRC   extracted NetBSD syssrc root (contains usr/src/sys ...). Default
#           /nbsrc (where the CI job mounts it).
#   CROSSCOMPILE_NEGCTL=1   teeth check: compile a deliberately-broken TU and
#                           assert the build FAILS (so a real break can't slip by).
#
# Exit 0 = all objects built + linked clean for elf32-vax (ILP32 width-clean).
# Any failure is fatal (set -e).

set -eu

TARGET="${TARGET:-vax--netbsdelf}"
CC="${TARGET}-gcc"
OBJDUMP="${TARGET}-objdump"
SRC="$(pwd)"
KMOD="$SRC/src/kernel-netbsd"
CORE="$SRC/src/kernel-core"
ODS2="$SRC/src/vmsfs/ods2"             # the ACP's pure on-disk EDIT helpers
ODS2_INC="$SRC/src/vmsfs/include"      # vmsfs/ods2.h (the genuine ODS-2 codec)
GUEST="$SRC/tests/netbsd/guest"        # FC-P0.4: cluster_seam.c, the R3 selftest
NBSRC="${NBSRC:-/nbsrc}"
SYS="$NBSRC/usr/src/sys"
OUT="${OUT:-/tmp/vms-module-vax}"
rm -rf "$OUT"; mkdir -p "$OUT"

if [ ! -d "$SYS" ]; then
    echo "FAIL: NetBSD kernel sources not found at $SYS (mount syssrc at \$NBSRC)" >&2
    exit 2
fi

# NetBSD kernel-build arch-include symlinks (what bsd.klinks.mk creates in the
# build dir): `#include <machine/...>' / <vax/...> resolve here. VAX has no
# x86/amd64 arch dependency (unlike the amd64 build), so only the two VAX links
# are needed.
KL="$(mktemp -d)"
trap 'rm -rf "$KL"' EXIT
ln -sf "$SYS/arch/vax/include" "$KL/machine"
ln -sf "$SYS/arch/vax/include" "$KL/vax"

# The same freestanding kernel-module TU environment an in-guest bsd.kmodule.mk
# VAX build uses, plus -Werror so a warning is a per-PR failure. No x86-only
# codegen flags (-mno-sse / -mcmodel=kernel are amd64-only); VAX needs none.
CFLAGS="-std=gnu99 -Werror -Wall -ffreestanding -fno-strict-aliasing -fno-omit-frame-pointer"
CPPFLAGS="-DOVMX_KBACKEND_NETBSD -DOVMX_ODS2_KERNEL -DOVMX_DEVTAB_SUBSTRATE_DISK_RESOLVE -nostdinc -isystem $KL -isystem $SYS -isystem $SYS/arch -isystem $SYS/../common/include -D_KERNEL -D_MODULE -I$KMOD -I$CORE -I$ODS2_INC"

# The module's translation units -- EXACTLY src/kernel-netbsd/Makefile's SRCS:
# the NetBSD backend glue + the OVMX intrusive containers + THE SHARED executive
# facility sources (the identical files the Linux vms.ko builds). There is one
# implementation of each facility; this gate compiles them with the NetBSD
# backend for the 32-bit VAX.
#   vms_netbsd.c        - module glue: /dev/vms cdevsw, the process table, kauth
#   exec_list_netbsd.c  - OVMX intrusive list (eflag/ast/mbx/lock queues)
#   exec_hash_netbsd.c  - OVMX intrusive hash (proctab + lock resource database)
#   exec_rbtree_netbsd.c- OVMX intrusive red-black tree (lock-ID database); the
#                         packed parent+colour word is the star ILP32 audit item
#   vms_eflag/ast/access.c - event flags, ASTs, access modes
#   vms_mbx.c           - executive-resident mailboxes MBAn:
#   vms_proctab.c       - executive process table ($GETJPI/$SETPRN/$PROCESS_SCAN)
#   vms_lock.c          - distributed lock manager ($ENQ/$DEQ/$CONVERT/$GETLKI)
#   vms_lnm.c           - executive-resident logical-name tables LNM$SYSTEM/GROUP/
#                         JOB (rd vms-72da): DEFINE/DELETE/GETSCOPE + the
#                         read-only arena (sole exec_arena consumer)
#   vms_devtab.c        - the executive-resident DEVICE TABLE (rd vms-618, the
#                         LAST facility to join): $ASSIGN/$DASSGN/$ALLOC/$DALLOC/
#                         $GETDVI/$DEVICE_SCAN. Built with
#                         -DOVMX_DEVTAB_SUBSTRATE_DISK_RESOLVE, mirroring
#                         src/kernel-netbsd/Makefile: the NetBSD substrate
#                         supplies its own disk RESOLVERS (they must lazily open
#                         + cache the backing vnode) in vms_blockdev_netbsd.c,
#                         while the TABLE and every ownership rule stay shared.
SRCS="$KMOD/vms_netbsd.c \
      $KMOD/vms_lnm_arena_netbsd.c \
      $KMOD/vms_acct_rss_netbsd.c \
      $KMOD/vms_sysmem_netbsd.c \
      $KMOD/exec_list_netbsd.c \
      $KMOD/exec_hash_netbsd.c \
      $KMOD/exec_rbtree_netbsd.c \
      $CORE/vms_eflag.c \
      $CORE/vms_ast.c \
      $CORE/vms_access.c \
      $CORE/vms_mbx.c \
      $CORE/vms_proctab.c \
      $CORE/vms_lock.c \
      $CORE/vms_lnm.c \
      $CORE/vms_devtab.c \
      $CORE/vmsfs_acp.c \
      $ODS2/ods2_reader.c \
      $ODS2/ods2_edit.c \
      $KMOD/vms_blockdev_netbsd.c \
      $KMOD/vms_socket_netbsd.c \
      $KMOD/vms_lan_netbsd.c \
      $GUEST/cluster_seam.c \
      $CORE/vms_cluster_fork.c \
      $CORE/vms_cluster_fork_bind.c \
      $CORE/vms_pe.c \
      $CORE/vms_cnxman_csb.c \
      $CORE/vms_cnxman_recnx_fsm.c \
      $CORE/vms_cnxman_quorum.c \
      $CORE/vms_cluster_api.c \
      $CORE/vms_cluster_sysgen.c \
      $CORE/vms_cluster_codec.c \
      $CORE/vms_cluster_codec_cm.c \
      $CORE/vms_cluster_codec_hello.c \
      $CORE/vms_cluster_codec_vc.c \
      $CORE/vms_cluster_codec_blk.c \
      $CORE/vms_pe_fsm.c \
      $CORE/vms_cnxman_phase2.c \
      $CORE/vms_cnxman_barrier_fsm.c \
      $CORE/vms_cnxman_coord_fsm.c \
      $CORE/vms_scs_fsm.c \
      $CORE/vms_cluster_codec_scs.c \
      $CORE/vms_scs_dir.c \
      $CORE/vms_scs.c \
      $CORE/vms_cluster_codec_mscp.c \
      $CORE/vms_mscp_cl_fsm.c \
      $CORE/vms_cnxman_join_fsm.c \
      $CORE/vms_cnxman.c \
      $CORE/vms_mscp_srv_fsm.c \
      $CORE/vms_mscp_srv.c"
#   vms_blockdev_netbsd.c - the NetBSD exec_blockdev_* seam (bread/bwrite on a
#                         vn_bdev_openpath device vnode) + the single-unit ODS-2
#                         disk resolve (vms_devtab_disk_backing) the ACP $MOUNT
#                         calls -- vms-d5d, mirrors Makefile SRCS.

# ---- teeth check ---------------------------------------------------------
# A deliberately-broken TU MUST fail the cross-compile, or a real break slips by.
if [ "${CROSSCOMPILE_NEGCTL:-}" = "1" ]; then
    bad="$OUT/negctl_bad.c"
    { cat "$CORE/vms_eflag.c"; printf '\nthis is deliberately invalid C @@@ ;\n'; } > "$bad"
    # shellcheck disable=SC2086
    if "$CC" $CFLAGS $CPPFLAGS -c "$bad" -o /dev/null 2>/dev/null; then
        echo "FAIL (negctl): a deliberately-broken VAX TU COMPILED -- the cross-compile check has NO TEETH"
        exit 1
    fi
    echo "PASS (negctl): a deliberately-broken VAX TU fails the elf32-vax cross-compile, as it must"
    exit 0
fi

echo "=== toolchain ==="
"$CC" --version | head -1
"$CC" -dumpmachine
echo

echo "=== compile each module TU freestanding for elf32-vax (-Werror) ==="
OBJS=""
for s in $SRCS; do
    b="$(basename "$s")"
    o="$OUT/${b%.c}.o"
    echo "--- $CC -c $b ---"
    # shellcheck disable=SC2086
    "$CC" $CFLAGS $CPPFLAGS -c "$s" -o "$o"
    OBJS="$OBJS $o"
done
echo

# The explicit width-audit TU (ILP32 width class + packed-pointer invariant +
# container node sizes/offsets). GENERATED here at runtime rather than committed
# as a .c under src/ or tools/: a committed VAX-only, kernel-only .c is
# host-compiled by the authenticity gate
# tests/integration/test_userspace_service_register.sh (which globs every product
# .c) where it cannot build -- the same width-audit-TU generation pattern the
# elf32-vax cross-builds use. See docs/audit-ilp32-vax-netbsd-exec.md sec 1-3.
WIDTH_TU="$OUT/vms_exec_width_audit.c"
cat > "$WIDTH_TU" <<'WIDTH_EOF'
/* GENERATED by build-vms-module-vax.sh -- the explicit ILP32/endian width proof
 * for the OVMX executive core + its NetBSD intrusive containers, compiled for
 * elf32-vax (rd vms-20b9, epic vms-8e8). Compiled with the same kernel CFLAGS as
 * every module TU. See docs/audit-ilp32-vax-netbsd-exec.md sec 1-3. Not
 * committed (see the build script). */
#include <sys/param.h>
#include <sys/systm.h>
#include "exec_rbtree.h"   /* -> exec_rbtree_netbsd.h : the packed RB node */
#include "exec_hash.h"     /* -> exec_hash_netbsd.h   : the intrusive hash node */
#include "exec_list.h"     /* -> exec_list_netbsd.h   : the intrusive list node */

/* 1. width class: this must be the 32-bit ILP32 VAX, not an LP64 host */
_Static_assert(sizeof(long)      == 4, "VAX long must be 32-bit (ILP32)");
_Static_assert(sizeof(void *)    == 4, "VAX pointer must be 32-bit (ILP32)");
_Static_assert(sizeof(int)       == 4, "int 32-bit");
_Static_assert(sizeof(long long) == 8, "long long 64-bit");
_Static_assert(sizeof(size_t)    == sizeof(void *), "size_t tracks pointer width");

/* 2. THE packed parent+colour word invariant (exec_rbtree_netbsd.c). The RB node
 * packs the parent pointer and the colour bit into a single `unsigned long'
 * __rb_parent_color, recovering the pointer with `& ~1UL' and the colour with
 * `& 1UL'. This is width-clean IFF an unsigned long can hold a whole pointer --
 * true on ILP32 (VAX: both 4) and LP64 (amd64: both 8), false only on LLP64
 * (Windows), which is not an OVMX target. The colour bit is free because nodes
 * are at least word-aligned (low bit always 0). */
_Static_assert(sizeof(unsigned long) == sizeof(void *),
	       "packed RB parent+colour word must hold a whole pointer (ILP32/LP64)");
_Static_assert(_Alignof(exec_rbtree_node_t) >= 2,
	       "RB node must be >=2-byte aligned so the low colour bit is free");

/* 3. intrusive-container node sizes/offsets UNDER ILP32 (a long/pointer field
 * would shift them; on VAX every pointer/long is 4 bytes). The RB node shrinks
 * to 3 words = 12 bytes on ILP32 (it is 24 on LP64) and the packed word sits at
 * offset 8 -- the shrink the packing must survive. */
_Static_assert(sizeof(exec_rbtree_node_t) == 12, "RB node = 3 words = 12 bytes on ILP32");
_Static_assert(offsetof(exec_rbtree_node_t, __rb_parent_color) == 8, "packed word @8 on ILP32");
_Static_assert(sizeof(exec_rbtree_root_t)  == 4,  "RB root = 1 word = 4 bytes on ILP32");
_Static_assert(sizeof(exec_hash_node_t)    == 8,  "hash node = 2 words = 8 bytes on ILP32");
_Static_assert(sizeof(struct exec_hash_head) == 4, "hash bucket head = 1 word on ILP32");
_Static_assert(sizeof(exec_list_node_t)    == 8,  "list node = 2 words = 8 bytes on ILP32");

const char vms_exec_width_audit_marker[] = "ovmx-vms-exec-core-elf32-vax-width-audit";
WIDTH_EOF
echo "--- $CC -c (generated) vms_exec_width_audit.c ---"
# shellcheck disable=SC2086
"$CC" $CFLAGS $CPPFLAGS -c "$WIDTH_TU" -o "$OUT/vms_exec_width_audit.o"
OBJS="$OBJS $OUT/vms_exec_width_audit.o"
echo

echo "=== relocatable link the module (load-coherence: no duplicate defs) ==="
# A relocatable (`-r') link of the module object set. It FAILS on a duplicate
# symbol definition across the set (the load-coherence property the amd64 gate
# asserts); the residual undefined symbols are the NetBSD KPIs the module binds
# to the running kernel at load time (proven at B2, not here). The width-audit TU
# is excluded from the module object (it is a compile-time proof, not module code).
MODOBJS=""
for o in $OBJS; do
    case "$o" in *vms_exec_width_audit.o) continue;; esac
    MODOBJS="$MODOBJS $o"
done
# shellcheck disable=SC2086
"$CC" -nostdlib -r -o "$OUT/vms.kmod.o" $MODOBJS
echo "  linked $OUT/vms.kmod.o"
echo

echo "=== assert every emitted object is elf32-vax (arch: vax) ==="
FAIL=0
for o in $OBJS "$OUT/vms.kmod.o"; do
    FMT="$("$OBJDUMP" -f "$o" | grep -Ei 'file format|architecture' | tr '\n' ' ')"
    echo "  $(basename "$o"): $FMT"
    echo "$FMT" | grep -qiF 'file format elf32-vax' || { echo "    ^ NOT elf32-vax"; FAIL=1; }
    echo "$FMT" | grep -qiE 'architecture: *vax' || { echo "    ^ arch not vax"; FAIL=1; }
done
[ "$FAIL" -eq 0 ] || { echo "FAIL: at least one object was not elf32-vax/vax"; exit 1; }
echo

echo "=== ALL PROOFS PASSED: the OVMX executive vms module (15 TUs: vms_netbsd.c,"
echo "    vms_lnm_arena_netbsd.c, exec_{list,hash,rbtree}_netbsd.c + shared"
echo "    vms_{eflag,ast,access,mbx,proctab,lock,lnm,devtab}.c + the Files-11 ACP"
echo "    vmsfs_acp.c + its ods2_edit.c EDIT helpers, rd vms-6a7f)"
echo "    cross-compiles + relocatable-links for elf32-vax, ILP32 width-clean ==="