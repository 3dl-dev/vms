#!/bin/sh
# mk_sockecho_native.sh - build SOCKECHO.EXE, the VMS-native sockets program the
# DECC$SHR sockets proof runs (rd vms-99f, tests/qemu/test_syssvc_decc_socket.c).
#
# The same recipe as tests/lab/ci6/mk_rmhold_native.sh: SOCKECHO.C is compiled to
# an object and LINK.EXE --executable --use DECC$SHR makes the image, so every
# socket call binds by symbol-vector index to the DECC$SHR the guest stages, and
# IMGACT.EXE (PT_INTERP) activates it exactly as a customer's program. ONLY
# DECC$SHR is --used: the program is plain BSD sockets, and the proof is that the
# C RTL alone carries its sockets to the executive.
#
# Compiled against MUSL's headers (DECC$SHR is musl): struct layouts and
# constants the image passes are the ones DECC$SHR reads. _FORTIFY_SOURCE off
# and no stack protector: a glibc-fortified call (__read_chk ...) exists in no
# OVMX shareable (mk_rmhold_native.sh documents the CI failure that taught this).
#
# Usage: mk_sockecho_native.sh <LINK.EXE> <out-SOCKECHO.EXE> <DECC$SHR.EXE>
# Env:   IMGACT_INTERP_PATH  bake THIS activator path (builds its own LINK.EXE)
#        CC                  compiler (default gcc); MUSL_INC musl include dir
set -e
LINK_EXE=${1:?usage: mk_sockecho_native.sh <LINK.EXE> <out-SOCKECHO.EXE> <DECC\$SHR.EXE>}
OUT=${2:?need output SOCKECHO.EXE path}
DECC_SHR=${3:?need DECC\$SHR.EXE}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
CC=${CC:-gcc}
WORK=${WORK:-/tmp/mk-sockecho}
mkdir -p "$WORK" "$(dirname "$OUT")"
[ -f "$DECC_SHR" ] || { echo "mk_sockecho: producer image not found: $DECC_SHR"; exit 1; }

MUSL_INC=${MUSL_INC:-$(echo /usr/include/*-linux-musl)}
if [ -d "$MUSL_INC" ]; then
    INC="-nostdinc -isystem $MUSL_INC -isystem $($CC -print-file-name=include)"
else
    # An alpine (musl) toolchain: its default headers ARE musl's.
    INC=""
fi
echo "mk_sockecho: cc SOCKECHO.C (headers: ${MUSL_INC:-default})"
# shellcheck disable=SC2086
$CC -fPIC -O2 -fno-builtin -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
    -D_DEFAULT_SOURCE $INC -x c -c -o "$WORK/SOCKECHO.o" "$HERE/SOCKECHO.C"

if [ -n "${IMGACT_INTERP_PATH:-}" ]; then
    LINK_EXE=$WORK/LINK.EXE
    echo "mk_sockecho: cc link.c with IMGACT_INTERP_PATH=$IMGACT_INTERP_PATH"
    $CC -std=gnu11 -O2 -I"$REPO/src/vmslink/include" \
        -DIMGACT_INTERP_PATH="$IMGACT_INTERP_PATH" \
        -o "$LINK_EXE" "$REPO/src/vmslink/link.c"
fi
[ -x "$LINK_EXE" ] || { echo "mk_sockecho: no LINK.EXE at $LINK_EXE"; exit 1; }
echo "mk_sockecho: LINK.EXE --executable --use DECC\$SHR -> $OUT"
"$LINK_EXE" --executable --use "$DECC_SHR" -o "$OUT" "$WORK/SOCKECHO.o"
chmod +x "$OUT"
INTERP=$(readelf -lW "$OUT" | sed -n 's/.*Requesting program interpreter: \([^]]*\)].*/\1/p')
[ -n "$INTERP" ] || { echo "mk_sockecho: FATAL: $OUT has no PT_INTERP (not an IMGACT-activated image)"; exit 1; }
if [ -n "${IMGACT_INTERP_PATH:-}" ] && [ "$INTERP" != "$IMGACT_INTERP_PATH" ]; then
    echo "mk_sockecho: FATAL: $OUT bakes activator '$INTERP', not '$IMGACT_INTERP_PATH'"
    exit 1
fi
echo "mk_sockecho: created $OUT (activator $INTERP)"
