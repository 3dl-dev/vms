#!/bin/sh
# mk_vmslnm_shr.sh — build recipe for LIBVMSLNM$SHR.EXE, the OVMX logical-name
# manager shareable image (bead vms-b65.3, pillar vms-ade). Third link in the b65
# lib-migration chain (vmsprocess -> libvms -> **vmslnm** -> vmsfs -> vmsrms -> DCL),
# following the template established by mk_vmsprocess_shr.sh (vms-b65.1).
#
# LIBVMSLNM$SHR.EXE is the src/vmslnm library (logical-name tables, translation,
# search-list resolution, default system logicals) linked by LINK.EXE — NO ld/ld.so
# — into a single OVMX shareable (ELF ET_DYN) that:
#   (a) carries a `.vms$sv` symbol vector exposing its logical-name universals
#       (lnm_init/shutdown/get_manager, lnm_create[_multi]/delete/translate[_iter],
#       lnm_find_table, lnm_enumerate, lnm_setup_defaults, and the lnm_table_* ops);
#   (b) binds its libc/pthread CALL imports (calloc/free/memcpy, strlen/strncpy/
#       strchr/strcasecmp/toupper, pthread_once, ttyname) to DECC$SHR via LINK.EXE
#       emit_shareable's cross-image import binding (vms-e65): PLT + import-GOT +
#       `.vms$imp`. STRICT link (no --allow-undefined): every import MUST bind.
#
# vmslnm is a NEAR-LEAF: it depends only on pthread (from DECC$SHR) — NOT on
# vmsprocess / vmsfs / libvms — and it defines NO __thread objects (readelf shows no
# .tdata/.tbss in any of its 4 objects), so it is NOT a TLS producer. That makes it
# the simplest migration so far: no PT_TLS, no --use LIBVMSSYS$SHR (no vms$/vms_kif_
# freestanding syscall imports), just a plain shareable over DECC$SHR.
#
# Composition: the 4 vmslnm library translation units, compiled -fPIC musl:
#   lnm_table.c lnm_translate.c lnm_client.c lnm_defaults.c
# Compile flags mirror the proven lib-shareable pattern (mk_vmsprocess_shr.sh):
#   -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics
#   -fno-builtin        : keep memcpy/strncpy/... as real CALL26 imports to DECC$SHR
#   -mno-outline-atomics: no __aarch64_* outline-atomic helpers DECC$SHR lacks
#
# Usage:  mk_vmslnm_shr.sh <LINK.EXE> <out-LIBVMSLNM$SHR.EXE> \
#             <DECC$SHR.EXE> [vmslnm-src-dir] [libvms-include-dir]
# Env:    CC (default gcc), GSMATCH (default LEQUAL,1,0)
#
# Must run in the arm64 musl container where DECC$SHR.EXE already exists
# (see CLAUDE.md test loop). aarch64-only for now.
set -e

LINK_EXE=${1:?usage: mk_vmslnm_shr.sh <LINK.EXE> <out> <DECC$SHR.EXE> [src-dir] [libvms-inc]}
OUT=${2:?usage: mk_vmslnm_shr.sh <LINK.EXE> <out> <DECC$SHR.EXE> [src-dir] [libvms-inc]}
DECC_SHR=${3:?need path to DECC\$SHR.EXE (the C run-time producer)}
HERE=$(cd "$(dirname "$0")" && pwd)                          # src/vmslink
SRC=${4:-$(cd "$HERE/../vmslnm" && pwd)}                     # src/vmslnm
LIBVMS_INC=${5:-$(cd "$HERE/../libvms/include" && pwd)}      # for ssdef.h / ovmx_layout.h
CC=${CC:-gcc}
GSMATCH=${GSMATCH:-LEQUAL,1,0}

[ -f "$DECC_SHR" ] || { echo "mk_vmslnm_shr: DECC\$SHR.EXE not found: $DECC_SHR"; exit 1; }
[ -d "$SRC" ]      || { echo "mk_vmslnm_shr: vmslnm src dir not found: $SRC"; exit 1; }

WORK=${WORK:-/tmp/mk-vmslnm-shr}
mkdir -p "$WORK"

CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics"
INCS="-I$SRC/include -I$LIBVMS_INC"

echo "mk_vmslnm_shr: LINK.EXE=$LINK_EXE  CC=$CC  GSMATCH=$GSMATCH"
echo "mk_vmslnm_shr: src=$SRC  DECC\$SHR=$DECC_SHR"

OBJS=""
for u in lnm_table lnm_translate lnm_client lnm_defaults; do
    echo "  cc $u.c"
    $CC $CFLAGS $INCS -c -o "$WORK/$u.o" "$SRC/$u.c"
    OBJS="$OBJS $WORK/$u.o"
done

# The logical-name universals LIBVMSLNM$SHR exports (every name is a non-static
# function defined in the 4 objects above). Each becomes a PROCEDURE universal in
# .vms$sv. Order is the append-only vector contract — DO NOT reorder or delete once
# consumers bind indices; only append (GSMATCH LEQUAL-compatible).
VEC="\
lnm_init=PROCEDURE,lnm_shutdown=PROCEDURE,lnm_get_manager=PROCEDURE,\
lnm_find_table=PROCEDURE,\
lnm_create=PROCEDURE,lnm_create_multi=PROCEDURE,lnm_delete=PROCEDURE,\
lnm_translate=PROCEDURE,lnm_translate_iterative=PROCEDURE,\
lnm_enumerate=PROCEDURE,lnm_setup_defaults=PROCEDURE,\
lnm_table_create=PROCEDURE,lnm_table_destroy=PROCEDURE,lnm_table_insert=PROCEDURE,\
lnm_table_lookup=PROCEDURE,lnm_table_remove=PROCEDURE,lnm_table_enumerate=PROCEDURE"

echo "mk_vmslnm_shr: LINK.EXE --shareable --use DECC\$SHR -> $OUT"
# STRICT (no --allow-undefined): every libc/pthread import MUST bind to DECC$SHR.
# shellcheck disable=SC2086
"$LINK_EXE" --shareable --use "$DECC_SHR" \
    --symbol-vector "$VEC" \
    --gsmatch "$GSMATCH" \
    -o "$OUT" $OBJS

echo "mk_vmslnm_shr: created $OUT"
