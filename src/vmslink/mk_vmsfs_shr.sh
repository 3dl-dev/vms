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
# Composition: the 5 vmsfs library translation units, compiled -fPIC musl:
#   vmsfs_translate.c vmsfs_version.c vmsfs_case.c vmsfs_protect.c vmsfs_device.c
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

OBJS=""
for u in vmsfs_translate vmsfs_version vmsfs_case vmsfs_protect vmsfs_device; do
    echo "  cc $u.c"
    $CC $CFLAGS $INCS -c -o "$WORK/$u.o" "$SRC/$u.c"
    OBJS="$OBJS $WORK/$u.o"
done

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
vmsfs_compose_ods2_candidates=PROCEDURE"

echo "mk_vmsfs_shr: LINK.EXE --shareable --use DECC\$SHR --use LIBVMSLNM\$SHR -> $OUT"
# STRICT (no --allow-undefined): every libc import MUST bind to DECC$SHR and every
# lnm_* import MUST bind to LIBVMSLNM$SHR.
# shellcheck disable=SC2086
"$LINK_EXE" --shareable --use "$DECC_SHR" --use "$LNM_SHR" \
    --symbol-vector "$VEC" \
    --gsmatch "$GSMATCH" \
    -o "$OUT" $OBJS

echo "mk_vmsfs_shr: created $OUT"
