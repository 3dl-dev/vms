#!/bin/sh
# mk_rmhold_native.sh - build RMHOLD.EXE, the OVMX side of the controlled
# lock-tree remaster arms (rd vms-c2b, rd vms-6ad1 RA7).
#
# Same toolchain as src/apps/rctest/mk_rc3.sh and src/apps/parts/mk_parts.sh:
# cc compiles RMHOLD.C, then LINK.EXE links it into a VMS-native ET_DYN
# executable (PT_INTERP=IMGACT.EXE, NO ld/ld.so, NO DT_NEEDED/DT_HASH) whose
# externals bind by symbol-vector index at activation.
#
# TWO producers, and only two: DECC$SHR.EXE owns the C-RTL calls (printf,
# memcpy, strlen) and LIBVMS$SHR.EXE owns sys$enqw / sys$getlkiw / sys$hiber
# (src/libvms/syssvc/sys_lock.c and sys_process.c are both in that
# shareable's manifest -- src/vmslink/mk_libvms_shr.sh). .vms$imp binds by
# symbol-vector INDEX, so the producers passed here must be the SAME copies
# the image will activate against on the target, exactly as
# mk_evacwl_native.sh's header warns.
#
# Usage: mk_rmhold_native.sh <LINK.EXE> <out-RMHOLD.EXE> <DECC$SHR.EXE> <LIBVMS$SHR.EXE> [src-dir]
set -e
LINK_EXE=${1:?usage: mk_rmhold_native.sh <LINK.EXE> <out-RMHOLD.EXE> <DECC$SHR> <LIBVMS$SHR> [src-dir]}
OUT=${2:?need output RMHOLD.EXE path}
DECC_SHR=${3:?need DECC\$SHR.EXE}
VMS_SHR=${4:?need LIBVMS\$SHR.EXE}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${5:-$HERE}
CC=${CC:-gcc}
[ -x "$LINK_EXE" ] || { echo "mk_rmhold: no LINK.EXE at $LINK_EXE"; exit 1; }
for f in "$DECC_SHR" "$VMS_SHR"; do
    [ -f "$f" ] || { echo "mk_rmhold: producer image not found: $f"; exit 1; }
done
[ -f "$SRC/RMHOLD.C" ] || { echo "mk_rmhold: RMHOLD.C not found in $SRC"; exit 1; }
REPO=$(cd "$HERE/../../.." && pwd)
WORK=${WORK:-/tmp/mk-rmhold}
mkdir -p "$WORK" "$(dirname "$OUT")"
CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector}"
DEFS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"
echo "mk_rmhold: cc RMHOLD.C"
# -x c: RMHOLD.C's VMS-canonical uppercase name is not the ".c" extension cc
# sniffs for, the same reason mk_evacwl_native.sh passes -xc for EVACWL.C.
# shellcheck disable=SC2086
$CC $CFLAGS $DEFS -I"$REPO/src/libvms/include" \
    -x c -c -o "$WORK/RMHOLD.o" "$SRC/RMHOLD.C"
echo "mk_rmhold: LINK.EXE --executable --use DECC\$SHR --use LIBVMS\$SHR -> $OUT"
"$LINK_EXE" --executable --use "$DECC_SHR" --use "$VMS_SHR" \
    -o "$OUT" "$WORK/RMHOLD.o"
chmod +x "$OUT"
# An image whose activator the host kernel cannot open exec-fails before any
# OVMX code runs (mk_evacwl_native.sh learned this in the lab): state the
# baked path, and refuse an image that carries none.
INTERP=$(readelf -lW "$OUT" | sed -n 's/.*Requesting program interpreter: \([^]]*\)].*/\1/p')
[ -n "$INTERP" ] || { echo "mk_rmhold: FATAL: $OUT has no PT_INTERP (not an IMGACT-activated image)"; exit 1; }
echo "mk_rmhold: created $OUT (activator $INTERP)"
