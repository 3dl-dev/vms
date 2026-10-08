#!/bin/sh
# mk_symset.sh - build SYMSET.EXE, the CLI-callback RUN target (rd vms-cded),
# with the same cc -> LINK.EXE toolchain as RC3.EXE (mk_rc3.sh); it imports
# LIB$SET_SYMBOL / LIB$GET_SYMBOL from LIBVMS$SHR.
# Usage: mk_symset.sh <LINK.EXE> <out-SYMSET.EXE> <SYSLIB-dir>
set -e
LINK_EXE=${1:?usage: mk_symset.sh <LINK.EXE> <out-SYMSET.EXE> <SYSLIB-dir>}
OUT=${2:?need output SYMSET.EXE path}
SYSLIB=${3:?need the SYSLIB dir holding DECC\$SHR.EXE and LIBVMS\$SHR.EXE}
HERE=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-gcc}
WORK=${WORK:-/tmp/mk-symset}
mkdir -p "$WORK"
CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics}"
DEFS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"
for s in 'DECC$SHR' 'LIBVMS$SHR'; do
    [ -f "$SYSLIB/$s.EXE" ] || { echo "mk_symset: missing $SYSLIB/$s.EXE"; exit 1; }
done
echo "mk_symset: cc symset.c"
# shellcheck disable=SC2086
$CC $CFLAGS $DEFS -x c -c -o "$WORK/symset.o" "$HERE/symset.c"
echo "mk_symset: LINK.EXE --executable --use DECC\$SHR --use LIBVMS\$SHR -> $OUT"
"$LINK_EXE" --executable --use "$SYSLIB/DECC\$SHR.EXE" --use "$SYSLIB/LIBVMS\$SHR.EXE" \
    -o "$OUT" "$WORK/symset.o"
echo "mk_symset: created $OUT"
