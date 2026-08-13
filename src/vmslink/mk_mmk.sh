#!/bin/sh
# mk_mmk.sh — build recipe for MMK.EXE, the MadGoat MMK ("make" for VMS) built
# AS a VMS-native EXECUTABLE image (bead vms-ec70, self-host spine #4). Mirrors
# mk_dcl.sh / mk_tcc.sh exactly: compile every TU with the proven freestanding-
# musl CFLAGS, then LINK.EXE --executable --use {DECC$SHR + the six OVMX
# shareables} -o MMK.EXE, activated by IMGACT.EXE.
#
# COMPOSITION (see tests/toolchain/CMakeLists.txt for the identical source list
# used by the host functional ctest toolchain-mmk-parse):
#   - 15 vendored MadGoat make-engine TUs (tests/corpus/tier3-mmk/), stock except
#     for tagged "OVMX (vms-ec70)" seams — grep the tag to see every edit;
#   - the vms-486 PARSE_TABLES.MAR -> C grammar (tests/libvms/mmk_parse_tables.c),
#     compiled -DOVMX_MMK_PRODUCTION so its transitions fire MMK's real
#     parse_store / parse_obj_store (not the vms-486 test probe);
#   - the OVMX companions (tests/corpus/tier3-mmk/ovmx/):
#       ovmx_mmk_compat.c   RTL call-arity forwarding wrappers,
#       ovmx_mmk_cld.c      compiles mmk_cld.cld at run time via cli$compile_cld,
#       ovmx_mmk_sp.c       subprocess boundary (NOACTION dry-run; real exec is
#                           the deferred DCL-subprocess drive — see the header),
#       ovmx_mmk_cms.c      honest CMS-not-available stubs,
#       ovmx_mmk_builtins.c _INSQUE/_REMQUE + sp_once/lib$find_image_symbol.
#
# The force-included ovmx_mmk_compat.h adapts the stock vendored source to the
# OVMX RTL (VMS storage-class keywords, struct/constant spellings, the va_count
# call-site counting macro, the RTL arity wrappers).  See its header for the full
# rationale; every OVMX-defined representation is labeled a design choice (Rule 8).
#
# link.c / imgact.c are the complete toolchain and are OUT of file-domain here —
# do NOT edit them.  The RTL entry points MMK imports (lib$table_parse,
# cli$compile_cld/cli$dcl_parse/cli$present/cli$get_value, lib$reset_vm_zone,
# sys$filescan, ...) are auto-exported in the producer shareables' symbol vectors
# (mk_*_shr.sh generate the vector from the objects' global symbols).
#
# Usage:  mk_mmk.sh <LINK.EXE> <out-MMK.EXE> \
#             <DECC$SHR.EXE> <LIBVMS$SHR.EXE> <LIBVMSPROCESS$SHR.EXE> \
#             <LIBVMSFS$SHR.EXE> <LIBVMSLNM$SHR.EXE> <LIBVMSRMS$SHR.EXE> \
#             <LIBVMSSYS$SHR.EXE> [repo-src-dir]
# Env:    CC (default gcc), CFLAGS (default aarch64 musl; x86_64 caller sets its own)
# Must run in the musl container where the producer .EXE already exist.
set -e

LINK_EXE=${1:?usage: mk_mmk.sh <LINK.EXE> <out> <DECC\$SHR> <LIBVMS\$SHR> <LIBVMSPROCESS\$SHR> <LIBVMSFS\$SHR> <LIBVMSLNM\$SHR> <LIBVMSRMS\$SHR> <LIBVMSSYS\$SHR> [repo-src]}
OUT=${2:?need output MMK.EXE path}
DECC_SHR=${3:?need DECC\$SHR.EXE}
VMS_SHR=${4:?need LIBVMS\$SHR.EXE}
PROC_SHR=${5:?need LIBVMSPROCESS\$SHR.EXE}
FS_SHR=${6:?need LIBVMSFS\$SHR.EXE}
LNM_SHR=${7:?need LIBVMSLNM\$SHR.EXE}
RMS_SHR=${8:?need LIBVMSRMS\$SHR.EXE}
SYS_SHR=${9:?need LIBVMSSYS\$SHR.EXE}
HERE=$(cd "$(dirname "$0")" && pwd)                      # src/vmslink
REPO_SRC=${10:-$(cd "$HERE/.." && pwd)}                  # src
REPO=$(cd "$REPO_SRC/.." && pwd)                         # repo root
CORPUS="$REPO/tests/corpus/tier3-mmk"
OVMX="$CORPUS/ovmx"
GRAMMAR="$REPO/tests/libvms/mmk_parse_tables.c"
CC=${CC:-gcc}

for f in "$DECC_SHR" "$VMS_SHR" "$PROC_SHR" "$FS_SHR" "$LNM_SHR" "$RMS_SHR" "$SYS_SHR"; do
    [ -f "$f" ] || { echo "mk_mmk: producer image not found: $f"; exit 1; }
done
[ -d "$CORPUS" ] || { echo "mk_mmk: MMK corpus not found: $CORPUS"; exit 1; }

WORK=${WORK:-/tmp/mk-mmk}
rm -rf "$WORK"; mkdir -p "$WORK"

# Embed mmk_cld.cld as a C string (mmk_cld_src.h), the same header the host
# ctest generates via gen_mmk_cld_src.cmake.
{
    echo "static const char mmk_cld_source[] ="
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/  "/' -e 's/$/\\n"/' "$CORPUS/mmk_cld.cld"
    echo ";"
} > "$WORK/mmk_cld_src.h"

CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics -U_FORTIFY_SOURCE}"
DEFS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D__CRTL_VER=80400000 -DOVMX_MMK"
INCS="-I$WORK -I$OVMX -I$CORPUS -I$REPO/tests/libvms \
-I$REPO_SRC/libvms/include -I$REPO_SRC/vmsrms/include -I$REPO_SRC/vmsfs/include \
-I$REPO_SRC/vmslnm/include -I$REPO_SRC/vmsprocess/include"
FINC="-include $OVMX/ovmx_mmk_compat.h"

CORPUS_TUS="mmk parse_descrip parse_objects build_target misc mem fileio symbols \
objects default_rules get_rdt readdesc str"
COMPANIONS="ovmx_mmk_cld ovmx_mmk_sp ovmx_mmk_cms ovmx_mmk_builtins"

echo "mk_mmk: CC=$CC  LINK.EXE=$LINK_EXE"
OBJS=""
for t in $CORPUS_TUS; do
    echo "  cc $t.c"
    $CC $CFLAGS $DEFS $FINC $INCS -c -o "$WORK/$t.o" "$CORPUS/$t.c"
    OBJS="$OBJS $WORK/$t.o"
done
echo "  cc mmk_parse_tables.c (OVMX_MMK_PRODUCTION — real parse_store)"
$CC $CFLAGS $DEFS -DOVMX_MMK_PRODUCTION $FINC $INCS -c -o "$WORK/mmk_parse_tables.o" "$GRAMMAR"
OBJS="$OBJS $WORK/mmk_parse_tables.o"
for t in $COMPANIONS; do
    echo "  cc $t.c"
    $CC $CFLAGS $DEFS $FINC $INCS -c -o "$WORK/$t.o" "$OVMX/$t.c"
    OBJS="$OBJS $WORK/$t.o"
done
# ovmx_mmk_compat.c carries #undefs for the wrapper macros, so it is safe to
# force-include the compat header uniformly with every other TU.
echo "  cc ovmx_mmk_compat.c (RTL arity wrappers)"
$CC $CFLAGS $DEFS $FINC $INCS -c -o "$WORK/ovmx_mmk_compat.o" "$OVMX/ovmx_mmk_compat.c"
OBJS="$OBJS $WORK/ovmx_mmk_compat.o"

NOBJ=$(echo $OBJS | wc -w)
echo "mk_mmk: $NOBJ objects compiled (13 corpus + grammar + 5 companions = 19 expected)"
[ "$NOBJ" -eq 19 ] || { echo "mk_mmk: FAIL: expected 19 objects, got $NOBJ"; exit 1; }

echo "mk_mmk: LINK.EXE --executable --use {7 producers} -> $OUT"
# shellcheck disable=SC2086
"$LINK_EXE" --executable \
    --use "$DECC_SHR" --use "$VMS_SHR" --use "$PROC_SHR" \
    --use "$FS_SHR" --use "$LNM_SHR" --use "$RMS_SHR" --use "$SYS_SHR" \
    -o "$OUT" $OBJS

echo "mk_mmk: created $OUT"
