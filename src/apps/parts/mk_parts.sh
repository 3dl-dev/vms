#!/bin/sh
# mk_parts.sh - build PARTS.EXE, the OVMX 0.2 demo application, as a VMS-native
# EXECUTABLE image (beads vms-e97 / vms-f20). Same toolchain as DCL.EXE
# (mk_dcl.sh): cc compiles the two PARTS translation units, then LINK.EXE
# links them into an ET_DYN executable (PT_INTERP=IMGACT.EXE, NO ld/ld.so, NO
# DT_NEEDED/DT_HASH) whose externals are bound at activation via .vms$imp.
#
# PARTS imports from exactly two producers directly:
#   - libc CALL imports (printf/snprintf, malloc/calloc/free, mem*/str*,
#     strcasecmp/strtoul, getenv, exit)                             -> DECC$SHR
#   - the RMS system services sys$create/$open/$connect/$put/$get/
#     $disconnect/$close                                            -> LIBVMSRMS$SHR
# LIBVMSRMS$SHR --use's DECC$SHR/LIBVMS$SHR/LIBVMSFS$SHR (and those pull
# LIBVMSLNM$SHR/LIBVMSPROCESS$SHR/LIBVMSSYS$SHR), so IMGACT loads the whole
# producer graph transitively at activation - all must be present in SYS$SHARE.
#
# Usage:  mk_parts.sh <LINK.EXE> <out-PARTS.EXE> \
#             <DECC$SHR.EXE> <LIBVMSRMS$SHR.EXE> [parts-src-dir] [repo-src-dir]
# Env:    CC (default gcc), CFLAGS (default aarch64 musl flags; the x86_64
#         caller sets CC + CFLAGS with -mtls-dialect=gnu2).
# Must run in the arm64/amd64 musl container where the producer .EXE exist.
set -e

LINK_EXE=${1:?usage: mk_parts.sh <LINK.EXE> <out-PARTS.EXE> <DECC$SHR> <LIBVMSRMS$SHR> [parts-src] [repo-src]}
OUT=${2:?need output PARTS.EXE path}
DECC_SHR=${3:?need DECC\$SHR.EXE}
RMS_SHR=${4:?need LIBVMSRMS\$SHR.EXE}
HERE=$(cd "$(dirname "$0")" && pwd)                       # src/apps/parts
SRC=${5:-$HERE}                                           # src/apps/parts
REPO_SRC=${6:-$(cd "$HERE/../../" && pwd)}                # src
CC=${CC:-gcc}

for f in "$DECC_SHR" "$RMS_SHR"; do
    [ -f "$f" ] || { echo "mk_parts: producer image not found: $f"; exit 1; }
done
[ -f "$SRC/parts.c" ]    || { echo "mk_parts: parts.c not found in $SRC"; exit 1; }
[ -f "$SRC/parts_db.c" ] || { echo "mk_parts: parts_db.c not found in $SRC"; exit 1; }

WORK=${WORK:-/tmp/mk-parts}
mkdir -p "$WORK"

CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics}"
DEFS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"
INCS="-I$SRC -I$REPO_SRC/vmsrms/include -I$REPO_SRC/libvms/include"

echo "mk_parts: LINK.EXE=$LINK_EXE  CC=$CC"
echo "mk_parts: --use DECC\$SHR LIBVMSRMS\$SHR"

OBJS=""
for t in parts parts_db; do
    echo "  cc $t.c"
    # -x c: PARTS sources are C even if a caller renames them with a .C suffix.
    $CC $CFLAGS $DEFS $INCS -x c -c -o "$WORK/$t.o" "$SRC/$t.c"
    OBJS="$OBJS $WORK/$t.o"
done

echo "mk_parts: LINK.EXE --executable --use {DECC\$SHR,LIBVMSRMS\$SHR} -> $OUT"
# shellcheck disable=SC2086
"$LINK_EXE" --executable \
    --use "$DECC_SHR" --use "$RMS_SHR" \
    -o "$OUT" $OBJS

echo "mk_parts: created $OUT"
