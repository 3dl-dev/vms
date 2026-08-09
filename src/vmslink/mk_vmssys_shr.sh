#!/bin/sh
# mk_vmssys_shr.sh — build recipe for LIBVMSSYS$SHR.EXE, the src/libvmssys
# (freestanding /dev/vms client + string/math/stdio/futex helpers) VMS-native
# shareable image (bead vms-b6a, pillar vms-ade). Extracted from the inline
# build steps `src/imgact/test/lib_build_graph.sh`'s build_producer_graph()
# carried directly (run_dcl_native.sh, run_login_native.sh) so there is ONE
# place that knows the LIBVMSSYS$SHR recipe instead of two copies drifting —
# the same drift risk mk_libvms_shr.sh's LIST comment calls out for its own
# translation-unit list.
#
# LIBVMSSYS$SHR.EXE exports the /dev/vms client entry points (vms_kif_*: open/
# enq/deq/convert/assign/dassgn/getdvi/setprn/getjpi/procscan/event-flag ops/
# AST ops (dclast/setast/deliverast)/
# devscan) plus vms_strlen — the FIRST producer in the b65/c39 graph (nothing
# else exports these; DCL's SHOW SYSTEM and LOGINOUT's identity establishment
# both import from it). Freestanding musl target: -fPIC -O2 -ffreestanding
# -fno-stack-protector -fno-builtin plus one target-specific codegen flag
# (see ARCH below); the syscall trampoline is compiled from
# arch/$ARCH/syscall.S. Both aarch64 and x86_64 (vms-6da; the mk_*_shr.sh
# recipes have taken ARCH/CFLAGS as env-overridable since vms-cb5f -- this
# is the one recipe vms-b6a extracted afterwards, so it hadn't picked up the
# convention yet).
#
# The vector always exports vms_kif_setident (identity establishment): the
# DCL-only test harness (run_dcl_native.sh) omits it via a narrower ad hoc
# vector because DCL never calls it, but any shared producer build (this
# script, used by both DCL.EXE and LOGINOUT.EXE) exports it unconditionally —
# an unused export is harmless (append-only vector, §3 docs/design-link-
# native-toolchain.md), an omitted one is a link failure for whichever
# consumer needed it.
#
# Usage:  mk_vmssys_shr.sh <LINK.EXE> <out-LIBVMSSYS$SHR.EXE> [libvmssys-src-dir] [extra-vec]
# Env:    CC (default gcc), GSMATCH (default LEQUAL,1,0), ARCH (default
#         aarch64; also x86_64 -- selects arch/$ARCH/syscall.S), CFLAGS
#         (env-overridable; default is the aarch64 flag set below so
#         standalone/aarch64 callers need not change -- the x86_64 caller
#         passes CFLAGS with -mtls-dialect=gnu2 in place of
#         -mno-outline-atomics, same convention as every other mk_*_shr.sh)
#
# Must run where the target musl toolchain + arch/$ARCH/syscall.S apply
# (CLAUDE.md test loop / the musl container for that arch).
set -e

LINK_EXE=${1:?usage: mk_vmssys_shr.sh <LINK.EXE> <out> [libvmssys-src] [extra-vec]}
OUT=${2:?usage: mk_vmssys_shr.sh <LINK.EXE> <out> [libvmssys-src] [extra-vec]}
HERE=$(cd "$(dirname "$0")" && pwd)                        # src/vmslink
SRC=${3:-$(cd "$HERE/../libvmssys" && pwd)}                # src/libvmssys
EXTRA_VEC=${4:-}
CC=${CC:-gcc}
GSMATCH=${GSMATCH:-LEQUAL,1,0}
ARCH=${ARCH:-aarch64}

[ -d "$SRC" ] || { echo "mk_vmssys_shr: libvmssys src dir not found: $SRC"; exit 1; }
[ -f "$SRC/arch/$ARCH/syscall.S" ] || { echo "mk_vmssys_shr: unsupported ARCH=$ARCH (no $SRC/arch/$ARCH/syscall.S)"; exit 1; }

WORK=${WORK:-/tmp/mk-vmssys-shr}
mkdir -p "$WORK"

CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-stack-protector -fno-builtin -mno-outline-atomics -U_FORTIFY_SOURCE}"
CFLAGS="$CFLAGS -I$SRC"

echo "mk_vmssys_shr: LINK.EXE=$LINK_EXE  CC=$CC  GSMATCH=$GSMATCH  ARCH=$ARCH"
echo "mk_vmssys_shr: src=$SRC"

OBJS=""
for c in vms_string vms_snprintf vms_futex vms_stdio vms_math vms_runtime_init vms_kif; do
    echo "  cc $c.c"
    $CC $CFLAGS -c -o "$WORK/$c.o" "$SRC/$c.c"
    OBJS="$OBJS $WORK/$c.o"
done
echo "  cc arch/$ARCH/syscall.S"
$CC -fPIC -c -o "$WORK/syscall.o" "$SRC/arch/$ARCH/syscall.S"
OBJS="$OBJS $WORK/syscall.o"

SYS_VEC="vms_strlen=PROCEDURE,vms_kif_open=PROCEDURE,vms_kif_enq=PROCEDURE,vms_kif_deq=PROCEDURE,vms_kif_convert=PROCEDURE,vms_kif_assign=PROCEDURE,vms_kif_dassgn=PROCEDURE,vms_kif_getdvi_chan=PROCEDURE,vms_kif_setprn=PROCEDURE,vms_kif_getjpi_self=PROCEDURE,vms_kif_getjpi_pid=PROCEDURE,vms_kif_getjpi_prcnam=PROCEDURE,vms_kif_procscan=PROCEDURE,vms_kif_setef=PROCEDURE,vms_kif_clref=PROCEDURE,vms_kif_readef=PROCEDURE,vms_kif_waitfr=PROCEDURE,vms_kif_wflor=PROCEDURE,vms_kif_wfland=PROCEDURE,vms_kif_ascefc=PROCEDURE,vms_kif_dacefc=PROCEDURE,vms_kif_dlcefc=PROCEDURE,vms_kif_devscan=PROCEDURE,vms_kif_getdvi_devnam=PROCEDURE,vms_kif_setident=PROCEDURE,vms_kif_dclast=PROCEDURE,vms_kif_setast=PROCEDURE,vms_kif_deliverast=PROCEDURE,vms_kif_lnm_define=PROCEDURE,vms_kif_lnm_delete=PROCEDURE,vms_kif_lnm_translate=PROCEDURE,vms_kif_register_continue=PROCEDURE,vms_kif_mbx_create=PROCEDURE,vms_kif_mbx_assign=PROCEDURE,vms_kif_mbx_delmbx=PROCEDURE,vms_kif_mbx_write=PROCEDURE,vms_kif_mbx_read=PROCEDURE,vms_kif_p0_map=PROCEDURE,vms_kif_p0_unmap=PROCEDURE,vms_kif_p1_map=PROCEDURE,vms_kif_enter_image=PROCEDURE,vms_kif_image_rundown=PROCEDURE,vms_kif_p1_protect=PROCEDURE"
if [ -n "$EXTRA_VEC" ]; then
    SYS_VEC="$SYS_VEC,$EXTRA_VEC"
fi

echo "mk_vmssys_shr: LINK.EXE --shareable -> $OUT"
# shellcheck disable=SC2086
"$LINK_EXE" --shareable \
    --symbol-vector "$SYS_VEC" \
    --gsmatch "$GSMATCH" \
    -o "$OUT" $OBJS

echo "mk_vmssys_shr: created $OUT"
