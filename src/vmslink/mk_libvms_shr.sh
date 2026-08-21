#!/bin/sh
# mk_libvms_shr.sh — build recipe for LIBVMS$SHR.EXE, the OVMX main runtime
# shareable image (bead vms-b65.2, pillar vms-ade). FIFTH and LARGEST link in the
# b65 lib-migration chain (vmsprocess -> vmslnm -> vmsfs -> **libvms** -> vmsrms ->
# DCL), following the template established by mk_vmsprocess_shr.sh (vms-b65.1),
# mk_vmslnm_shr.sh (vms-b65.3) and mk_vmsfs_shr.sh (vms-b65.4).
#
# LIBVMS$SHR.EXE is the src/libvms library — the VMS system services ($ASSIGN/$QIO/
# $ENQ/$DEQ/$GETTIM/$CREMBX/... in syssvc/) and the run-time library (lib$/str$/
# mth$/ots$ in rtl/, plus descrip/status) — linked by LINK.EXE (NO ld/ld.so) into a
# single OVMX shareable (ELF ET_DYN) that:
#   (a) carries a `.vms$sv` symbol vector exposing ALL its universals — every
#       non-static function defined by its translation units (sys$*, lib$*, str$*,
#       mth$*, ots$*, ovmx_* ...), enumerated from `nm` of the compiled objects.
#       The vector is generated (not hand-typed): the objects define hundreds of
#       universals; a name-sorted PROCEDURE vector is deterministic across builds.
#       NOTE (follow-up): this milestone exports PROCEDURE universals only. libvms
#       DATA universals (global tables a future consumer might import) and a frozen,
#       append-only hand-curated vector for cross-build GSMATCH stability are
#       deferred — the b65.2 consumer imports a procedure (lib$extzv);
#   (b) binds its imports across FOUR OVMX producers via LINK.EXE emit_shareable's
#       cross-image import binding (vms-e65: PLT + import-GOT + `.vms$imp`):
#         - libc/libm CALL imports (malloc/memcpy/str*/snprintf/printf/the libm
#           transcendentals/fork/exec*/mmap/glob/time*/timer_*/...) AND the
#           stdin/stdout/stderr DATA imports  -> DECC$SHR;
#         - vms_pcb_get/init/set_identity/set_default_dir                -> LIBVMSPROCESS$SHR;
#         - vms_kif_open/enq/deq/convert (the /dev/vms lock-manager ioctls) -> LIBVMSSYS$SHR;
#         - vmsfs_to_linux_path (VMS filespec -> Linux path)             -> LIBVMSFS$SHR.
#       STRICT link (no --allow-undefined): every import MUST bind. libvms does NOT
#       import lnm_* directly (its logical-name path goes through vmsfs), so it does
#       NOT --use LIBVMSLNM$SHR — but LIBVMSFS$SHR does, so IMGACT loads LIBVMSLNM$SHR
#       transitively at activation (it must be present in SYS$SHARE).
#   (c) is a TLS producer: rtl/lib_signal.c defines ONE __thread object, so
#       LIBVMS$SHR.EXE carries PT_TLS + .vms$tls (like LIBVMSPROCESS$SHR, unlike
#       LIBVMSLNM$SHR/LIBVMSFS$SHR). Exactly ONE TLS object -> the VMS-native
#       single-object-TLS path (vms-b65.1); general multi-object-per-image TLS is
#       still vms-212. The activation loads TWO TLS producers (LIBVMSPROCESS$SHR +
#       LIBVMS$SHR); IMGACT's assign_tls_offsets gives each module its own TP offset.
#
# Composition: the libvms translation units listed in LIST below (see its comment --
# that list is NOT checked against src/libvms/CMakeLists.txt; vms-79f),
# compiled -fPIC musl. Compile flags mirror the proven lib-shareable pattern:
#   -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics
#   -fno-builtin        : keep memcpy/strncpy/... as real CALL26 imports to DECC$SHR
#   -mno-outline-atomics: no __aarch64_* outline-atomic helpers DECC$SHR lacks
# linux-headers MUST be installed in the build container: syssvc/sys_uring.c
# includes <linux/io_uring.h> and defines its vms_uring_* helpers internally.
#
# Usage:  mk_libvms_shr.sh <LINK.EXE> <out-LIBVMS$SHR.EXE> \
#             <DECC$SHR.EXE> <LIBVMSPROCESS$SHR.EXE> <LIBVMSSYS$SHR.EXE> \
#             <LIBVMSFS$SHR.EXE> [libvms-src-dir]
# Env:    CC (default gcc), GSMATCH (default LEQUAL,1,0)
#
# Must run in the arm64 musl container where the four producer .EXE already exist
# (see CLAUDE.md test loop). CC/CFLAGS are env-overridable (default aarch64
# musl flags); the x86_64 caller (vms-cb5f) sets CC + CFLAGS (with
# -mtls-dialect=gnu2, no -mno-outline-atomics) before invoking this script --
# this library is a TLS producer (rtl/lib_signal.c __thread).
set -e

LINK_EXE=${1:?usage: mk_libvms_shr.sh <LINK.EXE> <out> <DECC$SHR> <LIBVMSPROCESS$SHR> <LIBVMSSYS$SHR> <LIBVMSFS$SHR> [src]}
OUT=${2:?usage: mk_libvms_shr.sh <LINK.EXE> <out> <DECC$SHR> <LIBVMSPROCESS$SHR> <LIBVMSSYS$SHR> <LIBVMSFS$SHR> [src]}
DECC_SHR=${3:?need path to DECC\$SHR.EXE (the C run-time producer)}
PROC_SHR=${4:?need path to LIBVMSPROCESS\$SHR.EXE (vms_pcb_* producer)}
SYS_SHR=${5:?need path to LIBVMSSYS\$SHR.EXE (vms_kif_* producer)}
FS_SHR=${6:?need path to LIBVMSFS\$SHR.EXE (vmsfs_to_linux_path producer)}
HERE=$(cd "$(dirname "$0")" && pwd)                       # src/vmslink
SRC=${7:-$(cd "$HERE/../libvms" && pwd)}                  # src/libvms
CC=${CC:-gcc}
GSMATCH=${GSMATCH:-LEQUAL,1,0}

for f in "$DECC_SHR" "$PROC_SHR" "$SYS_SHR" "$FS_SHR"; do
    [ -f "$f" ] || { echo "mk_libvms_shr: producer image not found: $f"; exit 1; }
done
[ -d "$SRC" ] || { echo "mk_libvms_shr: libvms src dir not found: $SRC"; exit 1; }

WORK=${WORK:-/tmp/mk-libvms-shr}
mkdir -p "$WORK"

CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics -U_FORTIFY_SOURCE}"
# Sibling include dirs: own headers + libvmssys (vms_kif.h) + vmsprocess/vmslnm/
# vmsfs/vmsrms public headers (prototypes referenced across the RTL/syssvc).
INCS="-I$SRC/include -I$HERE/../libvmssys \
-I$HERE/../vmsprocess/include -I$HERE/../vmslnm/include \
-I$HERE/../vmsfs/include -I$HERE/../vmsrms/include \
-I$HERE/../vmsscs/include \
-I$HERE/include"   # ovmx_image.h/ovmx_symvec.h for sys_imgact + imgact_prodreg (vms-db2)
                   # vmsscs/include: scs_membership.h for F$GETSYI CLUSTER_* (vms-8d4)

echo "mk_libvms_shr: LINK.EXE=$LINK_EXE  CC=$CC  GSMATCH=$GSMATCH"
echo "mk_libvms_shr: src=$SRC"
echo "mk_libvms_shr: --use $DECC_SHR $PROC_SHR $SYS_SHR $FS_SHR"

# The translation units of the libvms library.
#
# THIS LIST IS A SECOND SOURCE OF TRUTH FOR src/libvms/CMakeLists.txt, AND
# NOTHING CHECKS THAT THE TWO AGREE. It used to claim "== src/libvms/
# CMakeLists.txt" in this comment; that claim was unenforced and it broke the
# first time someone added a TU. vms-2f8 added rtl/rightslist.c to CMakeLists
# and not here, everything built and every host test passed, and the ONLY
# thing that noticed was the VMS-native DCL link job in CI failing on an
# undefined rightslist_name_to_value five minutes later.
#
# The claim is deleted rather than reworded, per the standing ruling. Making
# the two lists provably agree is filed as vms-79f.
LIST="descrip status \
syssvc/sys_assign syssvc/sys_mailbox syssvc/sys_qio syssvc/sys_uring syssvc/sys_event \
syssvc/sys_time syssvc/sys_process syssvc/sys_memory syssvc/sys_logical syssvc/sys_ast \
syssvc/sys_lock syssvc/sys_misc syssvc/sys_security syssvc/sys_fao syssvc/sys_msg \
syssvc/sys_filescan \
syssvc/sys_float syssvc/sys_uai syssvc/sys_device syssvc/sys_operator syssvc/sys_condition \
syssvc/sys_imgact syssvc/imgact_prodreg \
rtl/ovmx_accounting rtl/lib_vm rtl/lib_output rtl/lib_signal rtl/lib_datetime rtl/lib_timer rtl/lib_misc \
rtl/lib_dyndesc rtl/lib_logical rtl/lib_symbol rtl/lib_string_ops rtl/lib_bitops \
rtl/lib_arith rtl/lib_charscan rtl/lib_queue rtl/lib_tree rtl/lib_tparse rtl/lib_common \
rtl/lib_eventflags rtl/str_routines rtl/mth_routines rtl/ots_routines rtl/sha256 \
rtl/lib_strconv rtl/lib_xlate_tables \
rtl/sysuaf rtl/rightslist rtl/rms_textfile rtl/str_util rtl/lib_cli rtl/purdy"

OBJS=""
for c in $LIST; do
    b=$(echo "$c" | tr / _)
    echo "  cc $c.c"
    $CC $CFLAGS $INCS -c -o "$WORK/$b.o" "$SRC/$c.c"
    OBJS="$OBJS $WORK/$b.o"
done

# Generate the symbol vector from the compiled objects. Every GLOBAL defined-text
# symbol (nm type 'T') becomes a PROCEDURE universal; every GLOBAL defined-DATA
# symbol (nm type 'D' initialized, 'B' bss/tentative, 'R' rodata) becomes a =DATA
# universal (vms-bd1/vms-b65.6). A cross-image consumer that GOT-imports one of
# these tables binds it to the producer's =DATA universal (vms-e65; link.c honors
# `=DATA`, exactly the stdin/stdout/stderr path). `$` in names (lib$/str$/...) is
# safe: it comes from command substitution, which is NOT re-expanded downstream.
#
# ORDER: FROZEN-ORDER-THEN-APPEND (bead vms-bd1). This was once emitted
# name-SORTED -- deterministic for a fixed object set, but NOT append-only: a new
# universal whose name sorts into the middle shifted the symbol-vector index of
# every later universal, breaking GSMATCH LEQUAL for any consumer (vmsrms, DCL)
# that had bound an index. Now the ORDER is frozen in the append-only manifest
# src/vmslink/libvms_shr.vec: every listed universal is emitted FIRST in manifest
# order (its line number IS its index), then any nm-discovered universal not yet
# in the manifest is APPENDED (sorted) after the frozen block -- so an established
# index is immovable. This is the OpenVMS upward-compatibility contract for a
# GSMATCH LEQUAL image (docs/design-link-native-toolchain.md §5, from the public
# VSI OpenVMS Linker Utility Manual -- clean-room, CLAUDE.md Rule 8).
MANIFEST=${LIBVMS_VEC:-$HERE/libvms_shr.vec}
[ -f "$MANIFEST" ] || { echo "mk_libvms_shr: FAIL: manifest not found: $MANIFEST"; exit 1; }

# Discovered universals: name=class, one per line, unique-sorted.
{ nm $OBJS 2>/dev/null | awk 'NF==3 && $2=="T"{print $3"=PROCEDURE"}'; \
  nm $OBJS 2>/dev/null | awk 'NF==3 && ($2=="D"||$2=="B"||$2=="R"){print $3"=DATA"}'; } \
    | sort -u > "$WORK/disc.vec"
[ -s "$WORK/disc.vec" ] || { echo "mk_libvms_shr: FAIL: empty symbol vector (no T/D symbols?)"; exit 1; }
cut -d= -f1 "$WORK/disc.vec" | sort -u > "$WORK/disc.names"

# Frozen manifest entries in file order (strip `#` comments + whitespace/blanks).
sed 's/#.*//' "$MANIFEST" | sed 's/[[:space:]]//g' | grep -v '^$' > "$WORK/frozen.vec"
cut -d= -f1 "$WORK/frozen.vec" | sort -u > "$WORK/frozen.names"

# Every frozen NON-retired universal must still be defined. A vanished one is
# refused LOUDLY: retire it in place (PRIVATE_PROCEDURE/PRIVATE_DATA) rather than
# delete the line, or every later index shifts.
MISSING=""
while IFS= read -r entry; do
    en=${entry%%=*}; ec=${entry#*=}
    case "$ec" in PRIVATE_*) continue ;; esac
    grep -qx "$en" "$WORK/disc.names" || MISSING="$MISSING $en"
done < "$WORK/frozen.vec"
if [ -n "$MISSING" ]; then
    echo "mk_libvms_shr: FAIL: frozen universal(s) no longer defined:$MISSING"
    echo "mk_libvms_shr:   RETIRE in place -- set the class to PRIVATE_PROCEDURE/PRIVATE_DATA"
    echo "mk_libvms_shr:   in $MANIFEST; NEVER delete the line, or every later index shifts."
    exit 1
fi

# Newly-discovered universals not yet frozen -> append (sorted) after the frozen
# block, keeping every frozen index immovable.
comm -23 "$WORK/disc.names" "$WORK/frozen.names" | sort > "$WORK/append.names"
# Emit each discovered `name=class` whose name is in the append set. This
# replaces `join -t= append.names disc.vec`, which is UNAVAILABLE in the
# alpine-musl native-link container (busybox has no `join`) -> Error 127 broke
# every LIBVMS$SHR-rebuilding job the moment the append set was non-empty
# (vms-5a2; bug latent behind vms-bd1's all-frozen state). awk set-membership is
# the portable equivalent: `disc.vec` is `sort -u`'d (sorted by name, names
# unique), so iterating it yields append entries in name-sorted order -- byte
# for byte the same as join's sorted-by-key output. busybox HAS comm+paste, so
# those lines stay.
awk -F= 'NR==FNR { want[$1]; next } ($1 in want)' \
    "$WORK/append.names" "$WORK/disc.vec" > "$WORK/append.vec"

VEC=$( cat "$WORK/frozen.vec" "$WORK/append.vec" | paste -sd, - )
[ -n "$VEC" ] || { echo "mk_libvms_shr: FAIL: empty symbol vector"; exit 1; }
NFROZEN=$(wc -l < "$WORK/frozen.vec" | tr -d ' ')
NAPPEND=$(wc -l < "$WORK/append.vec" | tr -d ' ')
NPROC=$(printf '%s' "$VEC" | tr ',' '\n' | grep -c '=PROCEDURE' || true)
NDATA=$(printf '%s' "$VEC" | tr ',' '\n' | grep -c '=DATA' || true)
echo "mk_libvms_shr: symbol vector: $NFROZEN frozen + $NAPPEND appended = $NPROC PROCEDURE + $NDATA DATA universals"
if [ "$NAPPEND" -gt 0 ]; then
    echo "mk_libvms_shr: NOTE: $NAPPEND universal(s) appended BEYOND the frozen manifest."
    echo "mk_libvms_shr:   Append these lines to $MANIFEST (and libvms_shr.vec.frozen) to lock their indices:"
    sed 's/^/    /' "$WORK/append.vec"
fi

echo "mk_libvms_shr: LINK.EXE --shareable --use {DECC\$SHR,LIBVMSPROCESS\$SHR,LIBVMSSYS\$SHR,LIBVMSFS\$SHR} -> $OUT"
# STRICT (no --allow-undefined): every libc/libm/DATA import MUST bind to DECC$SHR,
# every vms_pcb_* to LIBVMSPROCESS$SHR, vms_kif_* to LIBVMSSYS$SHR,
# vmsfs_to_linux_path to LIBVMSFS$SHR.
# shellcheck disable=SC2086
"$LINK_EXE" --shareable \
    --use "$DECC_SHR" --use "$PROC_SHR" --use "$SYS_SHR" --use "$FS_SHR" \
    --symbol-vector "$VEC" \
    --gsmatch "$GSMATCH" \
    -o "$OUT" $OBJS

echo "mk_libvms_shr: created $OUT"
