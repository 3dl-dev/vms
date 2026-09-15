#!/bin/sh
# build-mbx-vax.sh - produce the two elf32-vax artifacts the mailbox runtime
# proof (rd vms-fe8, parent vms-945e -> vms-476, epic vms-8e8) loads on real
# NetBSD/vax under SIMH:
#
#   1. vms.kmod  - the OVMX executive `vms' LOADABLE kernel module for
#                  NetBSD/vax, IDENTICAL in shape to the one B1's per-PR gate
#                  (build-vms-module-vax.sh) proves width-clean, P4-B
#                  (build-devvms-vax.sh) loads, P4-E (build-eflag-vax.sh)
#                  loads, and the proctab proof (build-proctab-vax.sh) loads --
#                  same 10 TUs, same -O2 -fno-pic loadable-module build, so
#                  this is the SAME module image, just rebuilt here so this
#                  proof owns its own cached artifact (mirrors how the
#                  proctab proof's proctab-artifacts is independent of P4-E's
#                  eflag-artifacts).
#   2. vmsmbx    - the mailbox guest tool
#                  (tests/netbsd/guest/vmsmbx.c), reaching /dev/vms THROUGH
#                  the NetBSD transport seam (kif_transport_netbsd.c),
#                  statically linked. This is the vax analogue of the
#                  NetBSD/amd64 P4-A mailbox tool build, and the SAME source
#                  build-mbx-tool-vax.sh (the fast per-PR link gate) proves
#                  builds+links -- this script additionally builds vms.kmod,
#                  which needs the pinned NetBSD kernel headers ($NBSRC) the
#                  per-PR gate deliberately does not require.
#
# WHY -O2 AND NOT B1's FLAGS. Same rationale as build-eflag-vax.sh /
# build-proctab-vax.sh / build-devvms-vax.sh: B1 is a width-audit GATE that
# permits unresolved symbols (relocatable `-r' link); a module that will
# actually `modload' cannot. This producer compiles at -O2 (as an in-guest
# bsd.kmodule.mk build would) and asserts every OVMX exec_*/vms_* symbol
# resolves -- only real NetBSD KPIs may be left undefined.
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
#   OUT     output dir for vms.kmod + vmsmbx. Default /out.
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
ODS2="$SRC/src/vmsfs/ods2"             # ACP on-disk EDIT helpers (vms-d5d)
ODS2_INC="$SRC/src/vmsfs/include"      # the genuine ODS-2 codec header vmsfs/ods2.h
LIBVMSSYS="$SRC/src/libvmssys"
PROBE="$SRC/tests/netbsd/guest"
NBSRC="${NBSRC:-/nbsrc}"
SYS="$NBSRC/usr/src/sys"
OUT="${OUT:-/out}"
# Remove ONLY this script's own outputs -- NOT the whole dir: run-mbx.sh
# shares OUT (mbx-artifacts) with the cached custom kernel (netbsd-OVMX), and
# wiping the dir here would delete that cache and force a ~40-min build.sh
# rerun.
mkdir -p "$OUT"; rm -f "$OUT"/vms.kmod "$OUT"/vmsmbx "$OUT"/*.o 2>/dev/null || true

[ -d "$SYS" ] || { echo "FAIL: NetBSD kernel sources not at $SYS (mount syssrc at \$NBSRC)" >&2; exit 2; }

# bsd.klinks.mk arch-include symlinks: <machine/...> / <vax/...> resolve here.
KL="$(mktemp -d)"
trap 'rm -rf "$KL"' EXIT
ln -sf "$SYS/arch/vax/include" "$KL/machine"
ln -sf "$SYS/arch/vax/include" "$KL/vax"

# Same freestanding kernel-module TU environment build-eflag-vax.sh /
# build-proctab-vax.sh / build-devvms-vax.sh use -- -O2 (kernel default) so
# the resource-hash inline uses link, -fno-pic so the object carries ABSOLUTE
# (R_VAX_32) relocations, not GOT-relative (R_VAX_GOT32) ones -- required for
# the vax kobj loader to modload it.
CFLAGS="-std=gnu99 -O2 -fno-pic -Werror -Wall -ffreestanding -fno-strict-aliasing -fno-omit-frame-pointer"
CPPFLAGS="-DOVMX_KBACKEND_NETBSD -DOVMX_ODS2_KERNEL -DOVMX_DEVTAB_SUBSTRATE_DISK_RESOLVE -nostdinc -isystem $KL -isystem $SYS -isystem $SYS/arch -isystem $SYS/../common/include -D_KERNEL -D_MODULE -I$KMOD -I$CORE -I$ODS2_INC"

# EXACTLY src/kernel-netbsd/Makefile's SRCS (= B1's SRCS = build-eflag-vax.sh's
# SRCS = build-proctab-vax.sh's SRCS = build-devvms-vax.sh's SRCS): the NetBSD
# backend glue + OVMX intrusive containers + the SHARED executive facility
# sources (vms_mbx.c is the one this proof exercises; the rest ride along
# because they are one module image).
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
      $PROBE/cluster_seam.c \
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
      $CORE/vms_dlm_ldwv.c \
      $CORE/vms_cnxman_barrier_fsm.c \
      $CORE/vms_cnxman_coord_fsm.c \
      $CORE/vms_scs_fsm.c \
      $CORE/vms_cluster_codec_scs.c \
      $CORE/vms_cluster_emit_guard.c \
      $CORE/vms_scs_dir.c \
      $CORE/vms_scs.c \
      $CORE/vms_cluster_codec_mscp.c \
      $CORE/vms_mscp_cl_fsm.c \
      $CORE/vms_cnxman_join_fsm.c \
      $CORE/vms_cnxman_diag.c \
      $CORE/vms_cnxman.c \
      $CORE/vms_mscp_srv_fsm.c \
      $CORE/vms_mscp_srv.c \
      $CORE/vms_mscp_srv_io.c \
      $CORE/vms_mscp_cl_io_fsm.c \
      $CORE/vms_mscp_cl_conn_fsm.c \
      $CORE/vms_mscp_cl.c \
      $CORE/vms_cluster_codec_dlm.c \
      $CORE/vms_dlm_scs_fsm.c \
      $CORE/vms_dlm_scs.c"

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

echo "=== build the mailbox guest tool (static elf32-vax) ==="
# Static so the guest needs no ld.elf_so / shared-lib staging to run it.
"$CC" -O -Wall -Wextra -static \
    -I"$LIBVMSSYS" -I"$KMOD" \
    -o "$OUT/vmsmbx" \
    "$PROBE/vmsmbx.c" "$LIBVMSSYS/kif_transport_netbsd.c"
"$OBJDUMP" -f "$OUT/vmsmbx" | grep -qiF 'file format elf32-vax' || { echo "FAIL: vmsmbx not elf32-vax"; exit 1; }
echo "  OK: vmsmbx (static elf32-vax)"
echo

echo "=== ARTIFACTS ==="
ls -l "$OUT/vms.kmod" "$OUT/vmsmbx"
echo "=== build-mbx-vax.sh: DONE (both elf32-vax artifacts ready for SIMH) ==="
