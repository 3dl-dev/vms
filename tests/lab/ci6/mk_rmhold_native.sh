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
# WHICH ACTIVATOR GETS BAKED, and it is the difference between an image that
# runs and one the host kernel cannot exec at all. PT_INTERP is baked by
# LINK.EXE at BUILD time and opened BY THE HOST KERNEL before any OVMX code
# runs, so it must name where the TARGET stages IMGACT.EXE:
#
#   a BOOTED node        /run/ovmx-boot/IMGACT.EXE          (PID 1 stages it there)
#   the QEMU harness     /vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE
#
# A passed-in LINK.EXE bakes whatever IT was built with -- build-static's bakes
# the BOOTED path -- so a caller whose target differs must set
# IMGACT_INTERP_PATH, and then this recipe builds its own linker with that path
# and REFUSES an image whose PT_INTERP came out as anything else. MEASURED, not
# theorised: without this the QEMU executive shard staged a perfectly good
# RMHOLD.EXE that exec-failed ENOENT, the child printed nothing, and five
# assertions in test_syssvc_ci6_rmhold failed with no visible cause -- the exact
# failure mk_evacwl_native.sh's own header warns about.
#
# Usage: mk_rmhold_native.sh <LINK.EXE> <out-RMHOLD.EXE> <DECC$SHR.EXE> <LIBVMS$SHR.EXE> [src-dir]
# Env:   IMGACT_INTERP_PATH  bake THIS activator path (builds its own LINK.EXE)
#        CC                  compiler for that linker (default gcc)
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
# _FORTIFY_SOURCE OFF, AND IT IS NOT A STYLE CHOICE. glibc's hardened headers
# rewrite printf() into __printf_chk(), a GLIBC-ONLY alias that no OVMX
# shareable exports and no VMS RTL has ever had -- so an image built on a
# distro that defaults _FORTIFY_SOURCE on fails to LINK at all:
#   %LINK-F-ERROR, unresolved external symbol '__printf_chk'
# which is exactly what the QEMU executive image's gcc did (CI, 2026-10-21),
# while the musl/alpine bootable build sailed through. .vms$imp binds by
# symbol-vector INDEX to the producers named here, so every external this
# object references has to be one of THEIR symbols.
DEFS="-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"
echo "mk_rmhold: cc RMHOLD.C"
# -x c: RMHOLD.C's VMS-canonical uppercase name is not the ".c" extension cc
# sniffs for, the same reason mk_evacwl_native.sh passes -xc for EVACWL.C.
# shellcheck disable=SC2086
$CC $CFLAGS $DEFS -I"$REPO/src/libvms/include" \
    -x c -c -o "$WORK/RMHOLD.o" "$SRC/RMHOLD.C"
# IMGACT_INTERP_PATH: build the linker that bakes THAT path (see the header).
if [ -n "${IMGACT_INTERP_PATH:-}" ]; then
    LINK_EXE=$WORK/LINK.EXE
    echo "mk_rmhold: cc link.c with IMGACT_INTERP_PATH=$IMGACT_INTERP_PATH"
    ${CC:-gcc} -std=gnu11 -O2 -I"$REPO/src/vmslink/include" \
        -DIMGACT_INTERP_PATH="$IMGACT_INTERP_PATH" \
        -o "$LINK_EXE" "$REPO/src/vmslink/link.c"
fi
echo "mk_rmhold: LINK.EXE --executable --use DECC\$SHR --use LIBVMS\$SHR -> $OUT"
"$LINK_EXE" --executable --use "$DECC_SHR" --use "$VMS_SHR" \
    -o "$OUT" "$WORK/RMHOLD.o"
chmod +x "$OUT"
# An image whose activator the host kernel cannot open exec-fails before any
# OVMX code runs (mk_evacwl_native.sh learned this in the lab): state the
# baked path, and refuse an image that carries none.
INTERP=$(readelf -lW "$OUT" | sed -n 's/.*Requesting program interpreter: \([^]]*\)].*/\1/p')
[ -n "$INTERP" ] || { echo "mk_rmhold: FATAL: $OUT has no PT_INTERP (not an IMGACT-activated image)"; exit 1; }
if [ -n "${IMGACT_INTERP_PATH:-}" ] && [ "$INTERP" != "$IMGACT_INTERP_PATH" ]; then
    echo "mk_rmhold: FATAL: $OUT bakes activator '$INTERP', the caller asked for"
    echo "  '$IMGACT_INTERP_PATH'. On the target the host kernel would fail to open"
    echo "  the activator and the image would never run -- a silent ENOENT before any"
    echo "  OVMX code, which is a suite failing for a reason nothing prints."
    exit 1
fi
echo "mk_rmhold: created $OUT (activator $INTERP)"
