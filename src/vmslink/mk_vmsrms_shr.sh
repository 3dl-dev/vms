#!/bin/sh
# mk_vmsrms_shr.sh — build recipe for LIBVMSRMS$SHR.EXE, the OVMX Record Management
# Services shareable image (bead vms-b65.5, pillar vms-ade). SIXTH and LAST library
# link in the b65 lib-migration chain before DCL
# (vmsprocess -> vmslnm -> vmsfs -> libvms -> **vmsrms** -> DCL), following the
# template established by mk_vmsprocess_shr.sh (vms-b65.1), mk_vmslnm_shr.sh
# (vms-b65.3), mk_vmsfs_shr.sh (vms-b65.4) and mk_libvms_shr.sh (vms-b65.2).
#
# LIBVMSRMS$SHR.EXE is the src/vmsrms library — VMS Record Management Services:
# the $OPEN/$CREATE/$CLOSE/$CONNECT/$GET/$PUT/$UPDATE/$DELETE/$FIND/$PARSE/$SEARCH
# file+record system services over sequential, relative and indexed file
# organizations (FAB/RAB/NAM/XAB control blocks) — linked by LINK.EXE (NO ld/ld.so)
# into a single OVMX shareable (ELF ET_DYN) that:
#   (a) carries a `.vms$sv` symbol vector exposing ALL its universals — every
#       non-static function defined by the 8 translation units: the RMS system
#       services (sys$open/sys$create/sys$close/sys$connect/sys$get/sys$put/
#       sys$update/sys$delete/sys$find/sys$parse/sys$search/sys$rewind/sys$flush/
#       sys$disconnect/sys$erase/sys$display) plus the org-specific record engines
#       (rms_seq_*/rms_rel_*/rms_idx_*) and the shared I/O helpers
#       (rms_read_exact/rms_write_exact). Enumerated from `nm` of the compiled
#       objects (type 'T'), name-sorted for a deterministic, cross-build-stable
#       vector — the same generation strategy as mk_libvms_shr.sh.
#   (b) binds its imports across THREE OVMX producers via LINK.EXE emit_shareable's
#       cross-image import binding (vms-e65: PLT + import-GOT + `.vms$imp`):
#         - libc CALL imports (malloc/calloc/free, mem*/str*/strtoul, snprintf,
#           the stdio FILE ops fopen/fread/fwrite/fclose, the POSIX file ops
#           open/close/read/write/lseek/stat/unlink/ftruncate/fsync/realpath, the
#           dir ops opendir/readdir/closedir, fnmatch, getenv/getuid/getgid,
#           __errno_location, pthread_mutex_lock/unlock)               -> DECC$SHR;
#         - vms$check_access (the libvms file-protection security service) -> LIBVMS$SHR;
#         - vmsfs_parse_filespec/compose_filespec/to_linux_path/to_vms_spec/
#           get_highest_version/mode_to_protection/protection_to_mode
#           (VMS filespec + version + protection translation)           -> LIBVMSFS$SHR.
#       STRICT link (no --allow-undefined): every import MUST bind. vmsrms does NOT
#       import lnm_* / vms_pcb_* / vms_kif_* directly, so it does NOT --use
#       LIBVMSLNM$SHR / LIBVMSPROCESS$SHR / LIBVMSSYS$SHR — but LIBVMSFS$SHR --use's
#       LIBVMSLNM$SHR and LIBVMS$SHR --use's LIBVMSPROCESS$SHR + LIBVMSSYS$SHR, so
#       IMGACT loads that whole graph TRANSITIVELY at activation (all producer
#       images must be present in SYS$SHARE).
#   (c) is NOT a TLS producer: none of the 8 vmsrms objects defines a __thread
#       object (readelf shows no .tdata/.tbss), so LIBVMSRMS$SHR.EXE carries NO
#       PT_TLS — like LIBVMSLNM$SHR and LIBVMSFS$SHR, unlike LIBVMSPROCESS$SHR and
#       LIBVMS$SHR. No single/multi-object TLS concern here (vms-212 N/A).
#
# Composition: the 8 vmsrms translation units (== src/vmsrms/CMakeLists.txt),
# compiled -fPIC musl. Compile flags mirror the proven lib-shareable pattern:
#   -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics
#   -fno-builtin        : keep memcpy/strncpy/... as real CALL26 imports to DECC$SHR
#   -mno-outline-atomics: no __aarch64_* outline-atomic helpers DECC$SHR lacks
#
# Usage:  mk_vmsrms_shr.sh <LINK.EXE> <out-LIBVMSRMS$SHR.EXE> \
#             <DECC$SHR.EXE> <LIBVMS$SHR.EXE> <LIBVMSFS$SHR.EXE> \
#             [vmsrms-src-dir] [libvms-include-dir] [vmsfs-include-dir]
# Env:    CC (default gcc), GSMATCH (default LEQUAL,1,0)
#
# Must run in the arm64 musl container where the three producer .EXE already exist
# (see CLAUDE.md test loop). CC/CFLAGS are env-overridable (default aarch64
# musl flags); the x86_64 caller (vms-cb5f) sets CC + CFLAGS (with
# -mtls-dialect=gnu2, no -mno-outline-atomics) before invoking this script.
set -e

LINK_EXE=${1:?usage: mk_vmsrms_shr.sh <LINK.EXE> <out> <DECC$SHR> <LIBVMS$SHR> <LIBVMSFS$SHR> [src] [libvms-inc] [vmsfs-inc]}
OUT=${2:?usage: mk_vmsrms_shr.sh <LINK.EXE> <out> <DECC$SHR> <LIBVMS$SHR> <LIBVMSFS$SHR> [src] [libvms-inc] [vmsfs-inc]}
DECC_SHR=${3:?need path to DECC\$SHR.EXE (the C run-time producer)}
VMS_SHR=${4:?need path to LIBVMS\$SHR.EXE (vms\$check_access producer)}
FS_SHR=${5:?need path to LIBVMSFS\$SHR.EXE (vmsfs_* producer)}
HERE=$(cd "$(dirname "$0")" && pwd)                          # src/vmslink
SRC=${6:-$(cd "$HERE/../vmsrms" && pwd)}                     # src/vmsrms
LIBVMS_INC=${7:-$(cd "$HERE/../libvms/include" && pwd)}      # ovmx_layout.h/ssdef.h/rmsdef.h
VMSFS_INC=${8:-$(cd "$HERE/../vmsfs/include" && pwd)}        # vmsfs/filespec.h, version.h
CC=${CC:-gcc}
GSMATCH=${GSMATCH:-LEQUAL,1,0}

for f in "$DECC_SHR" "$VMS_SHR" "$FS_SHR"; do
    [ -f "$f" ] || { echo "mk_vmsrms_shr: producer image not found: $f"; exit 1; }
done
[ -d "$SRC" ] || { echo "mk_vmsrms_shr: vmsrms src dir not found: $SRC"; exit 1; }

WORK=${WORK:-/tmp/mk-vmsrms-shr}
mkdir -p "$WORK"

CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics -U_FORTIFY_SOURCE}"
INCS="-I$SRC/include -I$LIBVMS_INC -I$VMSFS_INC"

echo "mk_vmsrms_shr: LINK.EXE=$LINK_EXE  CC=$CC  GSMATCH=$GSMATCH"
echo "mk_vmsrms_shr: src=$SRC"
echo "mk_vmsrms_shr: --use $DECC_SHR $VMS_SHR $FS_SHR"

# The 8 translation units of the vmsrms library (== src/vmsrms/CMakeLists.txt).
LIST="rms_core rms_seq rms_rel rms_idx rms_record rms_parse rms_search rms_util"

OBJS=""
for c in $LIST; do
    echo "  cc $c.c"
    $CC $CFLAGS $INCS -c -o "$WORK/$c.o" "$SRC/$c.c"
    OBJS="$OBJS $WORK/$c.o"
done

# Generate the PROCEDURE symbol vector from the compiled objects: every GLOBAL
# defined-text symbol (nm type 'T') becomes a universal, name-sorted for a
# deterministic vector. `$` in the sys$* names is safe: it comes from command
# substitution, which is NOT re-expanded when the value is used.
VEC=$(nm $OBJS 2>/dev/null | awk 'NF==3 && $2=="T"{print $3}' | sort -u \
      | sed 's/$/=PROCEDURE/' | paste -sd, -)
[ -n "$VEC" ] || { echo "mk_vmsrms_shr: FAIL: empty symbol vector (no T symbols?)"; exit 1; }
NVEC=$(printf '%s' "$VEC" | tr ',' '\n' | grep -c '=PROCEDURE' || true)
echo "mk_vmsrms_shr: symbol vector has $NVEC PROCEDURE universals"

echo "mk_vmsrms_shr: LINK.EXE --shareable --use {DECC\$SHR,LIBVMS\$SHR,LIBVMSFS\$SHR} -> $OUT"
# STRICT (no --allow-undefined): every libc import MUST bind to DECC$SHR,
# vms$check_access to LIBVMS$SHR, every vmsfs_* to LIBVMSFS$SHR.
# shellcheck disable=SC2086
"$LINK_EXE" --shareable \
    --use "$DECC_SHR" --use "$VMS_SHR" --use "$FS_SHR" \
    --symbol-vector "$VEC" \
    --gsmatch "$GSMATCH" \
    -o "$OUT" $OBJS

echo "mk_vmsrms_shr: created $OUT"
