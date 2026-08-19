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
# vmslnm defines NO __thread objects (readelf shows no .tdata/.tbss in any of its 4
# objects), so it is NOT a TLS producer: no PT_TLS.
#
# It DOES --use LIBVMSSYS$SHR now (vms-96e2): LNM$SYSTEM is executive-resident
# (vms-d37/#193), so lnm_client.c / lnm_translate.c call vms_kif_lnm_define/_delete/
# _translate (the /dev/vms client in libvmssys). Those are CALL imports that must
# bind cross-image to LIBVMSSYS$SHR (which exports them in its .vms$sv) — exactly as
# the libc imports bind to DECC$SHR. LIBVMSSYS$SHR is located next to DECC$SHR in
# the same system-image dir (override with $VMSSYS_SHR); it is freestanding, so it
# adds no further transitive producer. STRICT link (no --allow-undefined): every
# import MUST bind, so the vms_kif_lnm_* imports fail the link loudly if the
# LIBVMSSYS$SHR vector ever drops them.
#
# Composition (VMS-native, no hand TU-list — vms-9d0, epic vms-a90, Rung 2 PILOT of
# docs/design-vms-native-shareable-build.md): the vmslnm translation units are NOT
# re-typed here. They are DERIVED from the ONE CMake source set — the
# set(VMSLNM_SOURCES ...) list that add_library(vmslnm ...) compiles in
# src/vmslnm/CMakeLists.txt — so this recipe can no longer drift from the dev build.
# Those TUs are compiled -fPIC musl into .OBJ, packed into an .OLB by LIBRARIAN.EXE,
# and LINK.EXE --shareable pulls exactly the members its --symbol-vector universals
# (+ transitive refs) require via selective library search (the Rung-1 capability,
# #659/vms-bf8): resolve_olbs seeds its unresolved set from the symbol vector, so a
# /SHAREABLE links from the .OLB ALONE with no explicit object TU list — the VMS way
# (Linker Utility Manual §1.2.3 selective search rooted at SYMBOL_VECTOR). "Which
# modules go in" is now DERIVED by LINK from the reference graph, not hand-listed.
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
# (see CLAUDE.md test loop). CC/CFLAGS are env-overridable (default aarch64
# musl flags); the x86_64 caller (vms-cb5f) sets CC + CFLAGS (with
# -mtls-dialect=gnu2, no -mno-outline-atomics) before invoking this script.
set -e

LINK_EXE=${1:?usage: mk_vmslnm_shr.sh <LINK.EXE> <out> <DECC$SHR.EXE> [src-dir] [libvms-inc]}
OUT=${2:?usage: mk_vmslnm_shr.sh <LINK.EXE> <out> <DECC$SHR.EXE> [src-dir] [libvms-inc]}
DECC_SHR=${3:?need path to DECC\$SHR.EXE (the C run-time producer)}
HERE=$(cd "$(dirname "$0")" && pwd)                          # src/vmslink
SRC=${4:-$(cd "$HERE/../vmslnm" && pwd)}                     # src/vmslnm
LIBVMS_INC=${5:-$(cd "$HERE/../libvms/include" && pwd)}      # for ssdef.h / ovmx_layout.h
VMSSYS_INC=$(cd "$HERE/../libvmssys" && pwd)                 # for vms_kif.h
# LIBVMSSYS$SHR.EXE (vms_kif_lnm_* producer, vms-96e2): defaults to the same
# system-image dir as DECC$SHR; override with $VMSSYS_SHR.
SYS_SHR=${VMSSYS_SHR:-"$(dirname "$DECC_SHR")/LIBVMSSYS\$SHR.EXE"}
CC=${CC:-gcc}
GSMATCH=${GSMATCH:-LEQUAL,1,0}

[ -f "$DECC_SHR" ] || { echo "mk_vmslnm_shr: DECC\$SHR.EXE not found: $DECC_SHR"; exit 1; }
[ -f "$SYS_SHR" ]  || { echo "mk_vmslnm_shr: LIBVMSSYS\$SHR.EXE not found: $SYS_SHR (vmslnm now imports vms_kif_lnm_*; build it first or set \$VMSSYS_SHR)"; exit 1; }
[ -d "$SRC" ]      || { echo "mk_vmslnm_shr: vmslnm src dir not found: $SRC"; exit 1; }

WORK=${WORK:-/tmp/mk-vmslnm-shr}
mkdir -p "$WORK"

CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics -U_FORTIFY_SOURCE}"
INCS="-I$SRC/include -I$LIBVMS_INC -I$VMSSYS_INC"

echo "mk_vmslnm_shr: LINK.EXE=$LINK_EXE  CC=$CC  GSMATCH=$GSMATCH"
echo "mk_vmslnm_shr: src=$SRC  DECC\$SHR=$DECC_SHR"

# ---------------------------------------------------------------------------
# TU set: DERIVED from the ONE CMake source list, never re-typed here (vms-9d0).
# Read set(VMSLNM_SOURCES ...) straight out of src/vmslnm/CMakeLists.txt — the
# same list add_library(vmslnm ...) compiles for the dev build — so the recipe
# and CMake cannot disagree. (Rung C.2's file(GENERATE) srclist is not yet in
# tree; reading the add_library list directly is the task's preferred option and
# keeps this migration to a single file.)
# ---------------------------------------------------------------------------
CMAKELISTS="$SRC/CMakeLists.txt"
[ -f "$CMAKELISTS" ] || { echo "mk_vmslnm_shr: CMake source list not found: $CMAKELISTS"; exit 1; }
UNITS=$(awk '/set\(VMSLNM_SOURCES/{f=1} f{print} /\)/{if(f)exit}' "$CMAKELISTS" \
        | grep -oE '[A-Za-z0-9_/]+\.c')
[ -n "$UNITS" ] || { echo "mk_vmslnm_shr: could not derive VMSLNM_SOURCES from $CMAKELISTS"; exit 1; }
NUNITS=$(printf '%s\n' $UNITS | grep -c '.')
echo "mk_vmslnm_shr: derived $NUNITS TU(s) from CMake VMSLNM_SOURCES: $(printf '%s ' $UNITS)"

# Build LIBRARIAN.EXE (the OVMX object-library utility, src/vmslink/librarian.c) —
# an ordinary host tool, same convention as the bootstrap LINK.EXE. It packs the
# .OBJ members into the .OLB that LINK.EXE then selectively searches.
echo "  cc LIBRARIAN.EXE"
$CC -std=gnu11 -O2 -I"$HERE/include" -I"$LIBVMS_INC" \
    -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -o "$WORK/LIBRARIAN.EXE" "$HERE/librarian.c"

# Compile each derived TU to a .OBJ.
OBJS=""
for c in $UNITS; do
    u=$(basename "$c" .c)
    echo "  cc $c"
    $CC $CFLAGS $INCS -c -o "$WORK/$u.OBJ" "$SRC/$c"
    OBJS="$OBJS $WORK/$u.OBJ"
done

# LIBRARIAN /CREATE the vmslnm object library from every derived .OBJ member.
OLB="$WORK/VMSLNM.OLB"
rm -f "$OLB"
echo "mk_vmslnm_shr: LIBRARIAN /CREATE $OLB from $NUNITS member(s)"
# shellcheck disable=SC2086
"$WORK/LIBRARIAN.EXE" /CREATE "$OLB" $OBJS

# The logical-name universals LIBVMSLNM$SHR exports (every name is a non-static
# function defined in the 4 objects above). Each becomes a PROCEDURE universal in
# .vms$sv. Order is the append-only vector contract — DO NOT reorder or delete once
# consumers bind indices; only append (GSMATCH LEQUAL-compatible).
#
# lnm_translate_values (vms-420) is APPENDED at the tail, not interleaved next to
# lnm_translate/lnm_translate_iterative above, precisely because of that append-only
# rule: DCL's cmd_show_logical calls it cross-shareable (the multi-value/search-list
# read that backs SHOW LOGICAL's display of every equivalence string, not just index
# 0), so a native LINK of DCL.EXE/LOGINOUT.EXE/etc. resolves it here or fails loudly
# with an unresolved external -- exactly the failure this comment exists to prevent
# a repeat of.
#
# lnm_translate_iterative is RETIRED IN PLACE (vms-240), NOT deleted: its slot keeps
# its position and becomes PRIVATE_PROCEDURE (OVMX_SV_RETIRED) so every later index
# (lnm_enumerate onward) stays put and GSMATCH LEQUAL,1,0 remains upward-compatible
# (VSI OpenVMS Linker Utility Manual; docs/design-link-native-toolchain.md
# §5.1/§5.3; same rule as mk_vmsprocess_shr.sh's eflag_* retirement). The C function
# is gone from lnm_translate.c, so the slot's value is left 0 and it is refused to
# any consumer (LINK.EXE find_universal / IMGACT sv_find_named skip retired slots).
# Its filespec-aware successor, lnm_translate_filespec (vms-240), is APPENDED at the
# tail -- $PARSE (LIBVMSRMS$SHR) binds it cross-image, so a native LINK resolves it
# here or fails loudly.
VEC="\
lnm_init=PROCEDURE,lnm_shutdown=PROCEDURE,lnm_get_manager=PROCEDURE,\
lnm_find_table=PROCEDURE,\
lnm_create=PROCEDURE,lnm_create_multi=PROCEDURE,lnm_delete=PROCEDURE,\
lnm_translate=PROCEDURE,lnm_translate_iterative=PRIVATE_PROCEDURE,\
lnm_enumerate=PROCEDURE,lnm_setup_defaults=PROCEDURE,\
lnm_table_create=PROCEDURE,lnm_table_destroy=PROCEDURE,lnm_table_insert=PROCEDURE,\
lnm_table_lookup=PROCEDURE,lnm_table_remove=PROCEDURE,lnm_table_enumerate=PROCEDURE,\
lnm_translate_values=PROCEDURE,\
lnm_translate_searchlist=PROCEDURE,\
lnm_translate_filespec=PROCEDURE,\
lnm_define_login_logicals=PROCEDURE"

echo "mk_vmslnm_shr: LINK.EXE --shareable --use {DECC\$SHR,LIBVMSSYS\$SHR} \\"
echo "               --symbol-vector ... SELECTIVE $OLB -> $OUT"
# VMS-native link: NO explicit object TU list. The .OLB is the pool; the
# --symbol-vector universals root the selective search (Rung-1 capability
# #659/vms-bf8) so LINK pulls exactly the members that define the universals +
# their transitive refs. STRICT (no --allow-undefined): every libc/pthread import
# MUST bind to DECC$SHR, every vms_kif_lnm_* import MUST bind to LIBVMSSYS$SHR
# (vms-96e2). Output captured so the Rung-0 reconcile can read the pull count.
set +e
"$LINK_EXE" --shareable --use "$DECC_SHR" --use "$SYS_SHR" \
    --symbol-vector "$VEC" \
    --gsmatch "$GSMATCH" \
    -o "$OUT" "$OLB" > "$WORK/link.out" 2>&1
LRC=$?
set -e
sed 's/^/   /' "$WORK/link.out"
[ "$LRC" -eq 0 ] || { echo "mk_vmslnm_shr: FAIL: LINK.EXE selective-pull link exited $LRC"; exit 1; }

# ---------------------------------------------------------------------------
# Rung-0 reconcile (vms-9d0): the .OLB + selective pull must produce the SAME
# module set the deleted hand-list linked whole. Every vmslnm TU defines at least
# one exported universal, so the correct result is "N of N members pulled": all
# members are pulled, none dropped. A DIFFERENT count means a module defines no
# universal and is not transitively referenced (a missing transitive ref or a
# genuinely dead TU) — investigate before trusting the migration, do NOT silently
# ship a smaller image than the hand-list did.
# ---------------------------------------------------------------------------
PULL=$(grep -oE "$OLB: [0-9]+ of [0-9]+ member" "$WORK/link.out" | grep -oE '[0-9]+ of [0-9]+' | head -1)
[ -n "$PULL" ] || { echo "mk_vmslnm_shr: FAIL: no selective-pull report for $OLB (is it an .OLB? did LINK search it?)"; exit 1; }
GOT=${PULL%% of *}
TOT=${PULL##* of }
if [ "$GOT" != "$TOT" ] || [ "$TOT" != "$NUNITS" ]; then
    echo "mk_vmslnm_shr: FAIL: Rung-0 reconcile: selective pull = '$PULL', expected '$NUNITS of $NUNITS'."
    echo "               The .OLB+symbol-vector pull differs from the hand-list (all $NUNITS TUs)."
    echo "               A vmslnm TU exports no universal and no other member references it — investigate."
    exit 1
fi
echo "mk_vmslnm_shr: Rung-0 reconcile OK: selective pull $PULL member(s) == the hand-list's whole set"

echo "mk_vmslnm_shr: created $OUT"
