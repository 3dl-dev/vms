#!/bin/sh
# mk_crtlshr_native.sh - build CRTLSHR.EXE (rd vms-569): CRTLSHR.C compiled by
# cc and linked by LINK.EXE --executable --use DECC$SHR --use LIBVMS$SHR, so it
# is an IMGACT-activated image (PT_INTERP = IMGACT.EXE) whose C RTL is the
# DECC$SHR it is activated against. test_syssvc_crtl_shr.c runs it from DCL.
#
# Usage: mk_crtlshr_native.sh <LINK.EXE> <out-CRTLSHR.EXE> <DECC$SHR> <LIBVMS$SHR>
# Env:   CC (default gcc), IMGACT_INTERP_PATH (the activator path to bake; the
#        recipe refuses an image that bakes another), WORK.
set -e
LINK_EXE=${1:?usage: mk_crtlshr_native.sh <LINK.EXE> <out-CRTLSHR.EXE> <DECC\$SHR> <LIBVMS\$SHR>}
OUT=${2:?need output CRTLSHR.EXE path}
DECC_SHR=${3:?need DECC\$SHR.EXE}
VMS_SHR=${4:?need LIBVMS\$SHR.EXE}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
CC=${CC:-gcc}
for f in "$DECC_SHR" "$VMS_SHR"; do
    [ -f "$f" ] || { echo "mk_crtlshr: producer image not found: $f"; exit 1; }
done
WORK=${WORK:-/tmp/mk-crtlshr}
mkdir -p "$WORK" "$(dirname "$OUT")"
echo "mk_crtlshr: cc CRTLSHR.C"
$CC -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -fdollars-in-identifiers \
    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -x c -c -o "$WORK/CRTLSHR.o" "$HERE/CRTLSHR.C"
if [ -n "${IMGACT_INTERP_PATH:-}" ]; then
    LINK_EXE=$WORK/LINK.EXE
    echo "mk_crtlshr: cc link.c with IMGACT_INTERP_PATH=$IMGACT_INTERP_PATH"
    ${CC} -std=gnu11 -O2 -I"$REPO/src/vmslink/include" \
        -DIMGACT_INTERP_PATH="$IMGACT_INTERP_PATH" \
        -o "$LINK_EXE" "$REPO/src/vmslink/link.c"
fi
[ -x "$LINK_EXE" ] || { echo "mk_crtlshr: no LINK.EXE at $LINK_EXE"; exit 1; }
echo "mk_crtlshr: LINK.EXE --executable --use DECC\$SHR --use LIBVMS\$SHR -> $OUT"
"$LINK_EXE" --executable --use "$DECC_SHR" --use "$VMS_SHR" -o "$OUT" "$WORK/CRTLSHR.o"
chmod +x "$OUT"
INTERP=$(readelf -lW "$OUT" | sed -n 's/.*Requesting program interpreter: \([^]]*\)].*/\1/p')
[ -n "$INTERP" ] || { echo "mk_crtlshr: FATAL: $OUT has no PT_INTERP (not an IMGACT-activated image)"; exit 1; }
if [ -n "${IMGACT_INTERP_PATH:-}" ] && [ "$INTERP" != "$IMGACT_INTERP_PATH" ]; then
    echo "mk_crtlshr: FATAL: $OUT bakes activator '$INTERP', not '$IMGACT_INTERP_PATH'"
    exit 1
fi
echo "mk_crtlshr: created $OUT (activator $INTERP)"
