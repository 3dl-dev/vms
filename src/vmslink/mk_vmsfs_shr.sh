#!/bin/sh
# mk_vmsfs_shr.sh — build recipe for LIBVMSFS$SHR.EXE, the OVMX VMS-filesystem
# shareable image (bead vms-b65.4, pillar vms-ade). Fourth link in the b65
# lib-migration chain (vmsprocess -> libvms -> vmslnm -> **vmsfs** -> vmsrms ->
# DCL), following the template established by mk_vmsprocess_shr.sh (vms-b65.1) and
# mk_vmslnm_shr.sh (vms-b65.3).
#
# LIBVMSFS$SHR.EXE is the src/vmsfs library (VMS filespec parsing/translation,
# device table, ODS-2 versioning, file protection, case-insensitive resolution)
# linked by LINK.EXE — NO ld/ld.so — into a single OVMX shareable (ELF ET_DYN)
# that:
#   (a) carries a `.vms$sv` symbol vector exposing its filesystem universals
#       (vmsfs_to_linux_path/to_vms_spec/parse_filespec/..., vmsfs_device_*,
#       vmsfs_*_version, vmsfs_*_protection, vmsfs_resolve_path_case, ...) —
#       vmsfs_to_linux_path is the one libvms (vms-b65.2) imports next;
#   (b) binds its libc CALL imports (memcpy/memset, str*/strn*case*cmp,
#       snprintf, atoi, qsort, isalnum/tolower/toupper, __errno_location, and the
#       dirent/stat/realpath/open/close/unlink file ops, pthread_mutex_lock/
#       unlock) to DECC$SHR, AND its logical-name imports (lnm_get_manager,
#       lnm_translate) to LIBVMSLNM$SHR — both via LINK.EXE emit_shareable's
#       cross-image import binding (vms-e65): PLT + import-GOT + `.vms$imp`.
#       STRICT link (no --allow-undefined): every import MUST bind.
#
# vmsfs depends on vmslnm (device→path translation consults the logical-name
# manager) and on libc, but NOT on vmsprocess / libvms. It defines NO __thread
# objects (readelf shows no .tdata/.tbss in any of its 5 objects), so
# LIBVMSFS$SHR.EXE is NOT a TLS producer (no PT_TLS) — like LIBVMSLNM$SHR and
# unlike LIBVMSPROCESS$SHR (vms-b65.1). No --use LIBVMSSYS$SHR either: vmsfs
# imports no vms$/vms_kif_ freestanding syscalls, only C-RTL + vmslnm.
#
# Composition (VMS-native, no hand TU-list — vms-71a3, epic vms-a90, Rung 3 of
# docs/design-vms-native-shareable-build.md, following the Rung-2 pilot
# e9512c76/mk_vmslnm_shr.sh): the vmsfs translation units are NOT re-typed
# here. They are DERIVED from the ONE CMake source set — the
# set(VMSFS_SOURCES ...) list that add_library(vmsfs ...) compiles in
# src/vmsfs/CMakeLists.txt — so this recipe can no longer drift from the dev
# build. Those TUs are compiled -fPIC musl into .OBJ, packed into an .OLB by
# LIBRARIAN.EXE, and LINK.EXE --shareable pulls exactly the members its
# --symbol-vector universals (+ transitive refs) require via selective library
# search (#659/vms-bf8): resolve_olbs seeds its unresolved set from the symbol
# vector, so a /SHAREABLE links from the .OLB ALONE with no explicit object TU
# list — the VMS way (Linker Utility Manual §1.2.3 selective search rooted at
# SYMBOL_VECTOR).
# Compile flags mirror the proven lib-shareable pattern (mk_vmslnm_shr.sh):
#   -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics
#   -fno-builtin        : keep memcpy/strncpy/... as real CALL26 imports to DECC$SHR
#   -mno-outline-atomics: no __aarch64_* outline-atomic helpers DECC$SHR lacks
#
# Usage:  mk_vmsfs_shr.sh <LINK.EXE> <out-LIBVMSFS$SHR.EXE> \
#             <DECC$SHR.EXE> <LIBVMSLNM$SHR.EXE> [vmsfs-src-dir] \
#             [libvms-include-dir] [vmslnm-include-dir]
# Env:    CC (default gcc), GSMATCH (default LEQUAL,1,0)
#
# Must run in the arm64 musl container where DECC$SHR.EXE and LIBVMSLNM$SHR.EXE
# already exist (see CLAUDE.md test loop). CC/CFLAGS are env-overridable
# (default aarch64 musl flags); the x86_64 caller (vms-cb5f) sets CC + CFLAGS
# (with -mtls-dialect=gnu2, no -mno-outline-atomics) before invoking this script.
set -e

LINK_EXE=${1:?usage: mk_vmsfs_shr.sh <LINK.EXE> <out> <DECC$SHR.EXE> <LIBVMSLNM$SHR.EXE> [src] [libvms-inc] [vmslnm-inc]}
OUT=${2:?usage: mk_vmsfs_shr.sh <LINK.EXE> <out> <DECC$SHR.EXE> <LIBVMSLNM$SHR.EXE> [src] [libvms-inc] [vmslnm-inc]}
DECC_SHR=${3:?need path to DECC\$SHR.EXE (the C run-time producer)}
LNM_SHR=${4:?need path to LIBVMSLNM\$SHR.EXE (the logical-name producer vmsfs imports)}
HERE=$(cd "$(dirname "$0")" && pwd)                          # src/vmslink
SRC=${5:-$(cd "$HERE/../vmsfs" && pwd)}                      # src/vmsfs
LIBVMS_INC=${6:-$(cd "$HERE/../libvms/include" && pwd)}      # for ssdef.h / ovmx_layout.h
LNM_INC=${7:-$(cd "$HERE/../vmslnm/include" && pwd)}         # for lnm.h (lnm_translate proto)
CC=${CC:-gcc}
GSMATCH=${GSMATCH:-LEQUAL,1,0}

[ -f "$DECC_SHR" ] || { echo "mk_vmsfs_shr: DECC\$SHR.EXE not found: $DECC_SHR"; exit 1; }
[ -f "$LNM_SHR" ]  || { echo "mk_vmsfs_shr: LIBVMSLNM\$SHR.EXE not found: $LNM_SHR"; exit 1; }
[ -d "$SRC" ]      || { echo "mk_vmsfs_shr: vmsfs src dir not found: $SRC"; exit 1; }

WORK=${WORK:-/tmp/mk-vmsfs-shr}
mkdir -p "$WORK"

CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics -U_FORTIFY_SOURCE}"
INCS="-I$SRC/include -I$LIBVMS_INC -I$LNM_INC"

echo "mk_vmsfs_shr: LINK.EXE=$LINK_EXE  CC=$CC  GSMATCH=$GSMATCH"
echo "mk_vmsfs_shr: src=$SRC  DECC\$SHR=$DECC_SHR  LIBVMSLNM\$SHR=$LNM_SHR"

# ---------------------------------------------------------------------------
# TU set: DERIVED from the ONE CMake source list, never re-typed here
# (vms-71a3). Read set(VMSFS_SOURCES ...) straight out of
# src/vmsfs/CMakeLists.txt — the same list add_library(vmsfs ...) compiles for
# the dev build — so the recipe and CMake cannot disagree.
# ---------------------------------------------------------------------------
CMAKELISTS="$SRC/CMakeLists.txt"
[ -f "$CMAKELISTS" ] || { echo "mk_vmsfs_shr: CMake source list not found: $CMAKELISTS"; exit 1; }
UNITS=$(awk '/set\(VMSFS_SOURCES/{f=1} f{print} /\)/{if(f)exit}' "$CMAKELISTS" \
        | grep -oE '[A-Za-z0-9_/]+\.c')
[ -n "$UNITS" ] || { echo "mk_vmsfs_shr: could not derive VMSFS_SOURCES from $CMAKELISTS"; exit 1; }
NUNITS=$(printf '%s\n' $UNITS | grep -c '.')
echo "mk_vmsfs_shr: derived $NUNITS TU(s) from CMake VMSFS_SOURCES: $(printf '%s ' $UNITS)"

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

# LIBRARIAN /CREATE the vmsfs object library from every derived .OBJ member.
OLB="$WORK/VMSFS.OLB"
rm -f "$OLB"
echo "mk_vmsfs_shr: LIBRARIAN /CREATE $OLB from $NUNITS member(s)"
# shellcheck disable=SC2086
"$WORK/LIBRARIAN.EXE" /CREATE "$OLB" $OBJS

# The VMS-filesystem universals LIBVMSFS$SHR exports (every name is a non-static
# function defined in the 5 objects above — enumerated with
# `nm *.o | awk '$2=="T"{print $3}'`, a superset of the public headers: it also
# exports the header-less but non-static helpers vmsfs_find_case_insensitive,
# vmsfs_is_valid_ods2_name, vmsfs_resolve_path_case, vmsfs_mode_to_protection,
# vmsfs_protection_to_mode). Each becomes a PROCEDURE universal in .vms$sv. Order
# is the append-only vector contract — DO NOT reorder or delete once consumers
# bind indices; only append (GSMATCH LEQUAL-compatible). vmsfs_to_linux_path is
# the universal libvms (vms-b65.2) binds next.
VEC="\
vmsfs_parse_filespec=PROCEDURE,vmsfs_compose_filespec=PROCEDURE,\
vmsfs_to_linux_path=PROCEDURE,vmsfs_to_vms_spec=PROCEDURE,\
vmsfs_resolve_device=PROCEDURE,vmsfs_translate_directory=PROCEDURE,\
vmsfs_wildcard_match=PROCEDURE,\
vmsfs_device_add=PROCEDURE,vmsfs_device_resolve=PROCEDURE,\
vmsfs_device_remove=PROCEDURE,vmsfs_device_count=PROCEDURE,\
vmsfs_get_highest_version=PROCEDURE,vmsfs_version_filename=PROCEDURE,\
vmsfs_create_new_version=PROCEDURE,vmsfs_purge_versions=PROCEDURE,\
vmsfs_list_versions=PROCEDURE,vmsfs_resolve_version=PROCEDURE,\
vmsfs_format_protection=PROCEDURE,vmsfs_parse_protection=PROCEDURE,\
vmsfs_mode_to_protection=PROCEDURE,vmsfs_protection_to_mode=PROCEDURE,\
vmsfs_find_case_insensitive=PROCEDURE,vmsfs_resolve_path_case=PROCEDURE,\
vmsfs_is_valid_ods2_name=PROCEDURE,\
vmsfs_device_concealed_rooted=PROCEDURE,\
vmsfs_resolve_filespec_device=PROCEDURE,\
vmsfs_compose_ods2_candidates=PROCEDURE,\
vmsfs_device_spec_kernel_mounted=PROCEDURE"

echo "mk_vmsfs_shr: LINK.EXE --shareable --use DECC\$SHR --use LIBVMSLNM\$SHR --symbol-vector ... SELECTIVE $OLB -> $OUT"
# VMS-native link: NO explicit object TU list. The .OLB is the pool; the
# --symbol-vector universals root the selective search (#659/vms-bf8) so LINK
# pulls exactly the members that define the universals + their transitive
# refs. STRICT (no --allow-undefined): every libc import MUST bind to DECC$SHR
# and every lnm_* import MUST bind to LIBVMSLNM$SHR. Output captured so the
# Rung-0 reconcile can read the pull count.
set +e
"$LINK_EXE" --shareable --use "$DECC_SHR" --use "$LNM_SHR" \
    --symbol-vector "$VEC" \
    --gsmatch "$GSMATCH" \
    -o "$OUT" "$OLB" > "$WORK/link.out" 2>&1
LRC=$?
set -e
sed 's/^/   /' "$WORK/link.out"
[ "$LRC" -eq 0 ] || { echo "mk_vmsfs_shr: FAIL: LINK.EXE selective-pull link exited $LRC"; exit 1; }

# ---------------------------------------------------------------------------
# Rung-0 reconcile (vms-71a3): the .OLB + selective pull must produce the SAME
# module set the deleted hand-list linked whole. Every vmsfs TU defines at
# least one exported universal, so the correct result is "N of N members
# pulled": all members are pulled, none dropped. A DIFFERENT count means a
# module defines no universal and is not transitively referenced -- investigate
# before trusting the migration, do NOT silently ship a smaller image than the
# hand-list did.
# ---------------------------------------------------------------------------
PULL=$(grep -oE "$OLB: [0-9]+ of [0-9]+ member" "$WORK/link.out" | grep -oE '[0-9]+ of [0-9]+' | head -1)
[ -n "$PULL" ] || { echo "mk_vmsfs_shr: FAIL: no selective-pull report for $OLB (is it an .OLB? did LINK search it?)"; exit 1; }
GOT=${PULL%% of *}
TOT=${PULL##* of }
if [ "$GOT" != "$TOT" ] || [ "$TOT" != "$NUNITS" ]; then
    echo "mk_vmsfs_shr: FAIL: Rung-0 reconcile: selective pull = '$PULL', expected '$NUNITS of $NUNITS'."
    echo "               The .OLB+symbol-vector pull differs from the hand-list (all $NUNITS TUs)."
    echo "               A vmsfs TU exports no universal and no other member references it -- investigate."
    exit 1
fi
echo "mk_vmsfs_shr: Rung-0 reconcile OK: selective pull $PULL member(s) == the hand-list's whole set"

echo "mk_vmsfs_shr: created $OUT"
