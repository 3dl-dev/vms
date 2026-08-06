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
# devscan) plus vms_strlen — the FIRST producer in the b65/c39 graph (nothing
# else exports these; DCL's SHOW SYSTEM and LOGINOUT's identity establishment
# both import from it). Freestanding musl target: -fPIC -O2 -ffreestanding
# -fno-stack-protector -fno-builtin -mno-outline-atomics, aarch64 syscall
# trampoline compiled from arch/aarch64/syscall.S (aarch64-only for now, per
# item vms-b6a; the x86_64 extension is vms-6da, a separate item, once wired).
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
# Env:    CC (default gcc), GSMATCH (default LEQUAL,1,0)
#
# aarch64 musl target only. Must run where the aarch64 musl toolchain +
# arch/aarch64/syscall.S apply (CLAUDE.md test loop / the arm64 musl container).
set -e

LINK_EXE=${1:?usage: mk_vmssys_shr.sh <LINK.EXE> <out> [libvmssys-src] [extra-vec]}
OUT=${2:?usage: mk_vmssys_shr.sh <LINK.EXE> <out> [libvmssys-src] [extra-vec]}
HERE=$(cd "$(dirname "$0")" && pwd)                        # src/vmslink
SRC=${3:-$(cd "$HERE/../libvmssys" && pwd)}                # src/libvmssys
EXTRA_VEC=${4:-}
CC=${CC:-gcc}
GSMATCH=${GSMATCH:-LEQUAL,1,0}

[ -d "$SRC" ] || { echo "mk_vmssys_shr: libvmssys src dir not found: $SRC"; exit 1; }

WORK=${WORK:-/tmp/mk-vmssys-shr}
mkdir -p "$WORK"

CFLAGS="-fPIC -O2 -ffreestanding -fno-stack-protector -fno-builtin -mno-outline-atomics -I$SRC"

echo "mk_vmssys_shr: LINK.EXE=$LINK_EXE  CC=$CC  GSMATCH=$GSMATCH"
echo "mk_vmssys_shr: src=$SRC"

OBJS=""
for c in vms_string vms_snprintf vms_futex vms_stdio vms_math vms_runtime_init vms_kif; do
    echo "  cc $c.c"
    $CC $CFLAGS -c -o "$WORK/$c.o" "$SRC/$c.c"
    OBJS="$OBJS $WORK/$c.o"
done
echo "  cc arch/aarch64/syscall.S"
$CC -fPIC -mno-outline-atomics -c -o "$WORK/syscall.o" "$SRC/arch/aarch64/syscall.S"
OBJS="$OBJS $WORK/syscall.o"

SYS_VEC="vms_strlen=PROCEDURE,vms_kif_open=PROCEDURE,vms_kif_enq=PROCEDURE,vms_kif_deq=PROCEDURE,vms_kif_convert=PROCEDURE,vms_kif_assign=PROCEDURE,vms_kif_dassgn=PROCEDURE,vms_kif_getdvi_chan=PROCEDURE,vms_kif_setprn=PROCEDURE,vms_kif_getjpi_self=PROCEDURE,vms_kif_getjpi_pid=PROCEDURE,vms_kif_getjpi_prcnam=PROCEDURE,vms_kif_procscan=PROCEDURE,vms_kif_setef=PROCEDURE,vms_kif_clref=PROCEDURE,vms_kif_readef=PROCEDURE,vms_kif_waitfr=PROCEDURE,vms_kif_wflor=PROCEDURE,vms_kif_wfland=PROCEDURE,vms_kif_ascefc=PROCEDURE,vms_kif_dacefc=PROCEDURE,vms_kif_dlcefc=PROCEDURE,vms_kif_devscan=PROCEDURE,vms_kif_getdvi_devnam=PROCEDURE,vms_kif_setident=PROCEDURE"
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
