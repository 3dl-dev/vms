#!/bin/sh
# mk_rc3.sh - build RC3.EXE, a minimal RUN target for the DCL $STATUS gate
# (vms-707). Same toolchain as mk_parts.sh: cc compiles rc3.c, then LINK.EXE
# links it into a VMS-native ET_DYN executable (PT_INTERP=IMGACT.EXE, NO
# ld/ld.so, NO DT_NEEDED/DT_HASH) whose libc externals bind to DECC$SHR at
# activation. Usage: mk_rc3.sh <LINK.EXE> <out-RC3.EXE> <DECC$SHR.EXE> [src-dir]
set -e
LINK_EXE=${1:?usage: mk_rc3.sh <LINK.EXE> <out-RC3.EXE> <DECC$SHR> [src-dir]}
OUT=${2:?need output RC3.EXE path}
DECC_SHR=${3:?need DECC\$SHR.EXE}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${4:-$HERE}
CC=${CC:-gcc}
[ -f "$DECC_SHR" ] || { echo "mk_rc3: producer image not found: $DECC_SHR"; exit 1; }
[ -f "$SRC/rc3.c" ] || { echo "mk_rc3: rc3.c not found in $SRC"; exit 1; }
WORK=${WORK:-/tmp/mk-rc3}
mkdir -p "$WORK"
CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics}"
DEFS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"
echo "mk_rc3: cc rc3.c"
# shellcheck disable=SC2086
$CC $CFLAGS $DEFS -x c -c -o "$WORK/rc3.o" "$SRC/rc3.c"
echo "mk_rc3: LINK.EXE --executable --use DECC\$SHR -> $OUT"
"$LINK_EXE" --executable --use "$DECC_SHR" -o "$OUT" "$WORK/rc3.o"
echo "mk_rc3: created $OUT"
