#!/bin/sh
# mk_vmsprocess_shr.sh — build recipe for LIBVMSPROCESS$SHR.EXE, the OVMX process-
# control shareable image (bead vms-b65.1, pillar vms-ade). This is the FIRST real
# OVMX-library migration onto the VMS-native toolchain (LINK.EXE + symbol vectors +
# IMGACT) and the TEMPLATE for the whole b65 chain (libvms -> vmslnm -> vmsfs ->
# vmsrms -> DCL).
#
# LIBVMSPROCESS$SHR.EXE is the src/vmsprocess library (PCBs, ASTs, event flags,
# access modes, process identity) linked by LINK.EXE — NO ld/ld.so — into a single
# OVMX shareable (ELF ET_DYN) that:
#   (a) carries a `.vms$sv` symbol vector exposing its process-control universals
#       (vms_pcb_*, ast_*, eflag_*, access_mode_*, priv_*, vms_*process/pid/uic);
#   (b) binds its libc/pthread CALL imports (pthread_mutex/cond_*, malloc/calloc/
#       free, memset/strncpy/snprintf/sscanf, getpid, close/raise/sigaction/
#       sigemptyset) to DECC$SHR via LINK.EXE emit_shareable's cross-image import
#       binding (vms-e65): PLT + import-GOT + `.vms$imp`;
#   (c) is a TLS producer (vms_pcb.c's `current_pcb` and vms_process.c's `proc` are
#       __thread) that coexists with the C-RTL's thread-pointer ownership via
#       IMGACT's absorb-producer-TLS path (vms-616).
#
# vms_uring_cleanup is referenced weak (io_uring cleanup lives in libvms); it
# correctly resolves to 0 here — vmsprocess proper never links the uring backend.
#
# Composition: the 5 vmsprocess translation units, compiled -fPIC musl:
#   vms_pcb.c vms_process.c ast.c event_flags.c access_modes.c
# Compile flags mirror the proven lib-shareable pattern
# (run_shareable_import_activation.sh / run_tls_producer_over_crtl.sh):
#   -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics
#   -fno-builtin        : keep memset/strncpy/... as real CALL26 imports to DECC$SHR
#   -mno-outline-atomics: no __aarch64_* outline-atomic helpers DECC$SHR lacks
#                         (vms_pcb_init's 4-byte __atomic CAS stays inline lock-free)
#
# Usage:  mk_vmsprocess_shr.sh <LINK.EXE> <out-LIBVMSPROCESS$SHR.EXE> \
#             <DECC$SHR.EXE> [LIBVMSSYS$SHR.EXE] [vmsprocess-src-dir] [libvms-include-dir]
# Env:    CC (default gcc), GSMATCH (default LEQUAL,1,0)
#
# Must run in the arm64 musl container where DECC$SHR.EXE already exists
# (see CLAUDE.md test loop). aarch64-only for now.
set -e

LINK_EXE=${1:?usage: mk_vmsprocess_shr.sh <LINK.EXE> <out> <DECC$SHR.EXE> [LIBVMSSYS$SHR.EXE] [src-dir] [libvms-inc]}
OUT=${2:?usage: mk_vmsprocess_shr.sh <LINK.EXE> <out> <DECC$SHR.EXE> [LIBVMSSYS$SHR.EXE] [src-dir] [libvms-inc]}
DECC_SHR=${3:?need path to DECC\$SHR.EXE (the C run-time producer)}
LIBVMSSYS_SHR=${4:-}
HERE=$(cd "$(dirname "$0")" && pwd)                          # src/vmslink
SRC=${5:-$(cd "$HERE/../vmsprocess" && pwd)}                 # src/vmsprocess
LIBVMS_INC=${6:-$(cd "$HERE/../libvms/include" && pwd)}      # for ssdef.h / prvdef.h
CC=${CC:-gcc}
GSMATCH=${GSMATCH:-LEQUAL,1,0}

[ -f "$DECC_SHR" ] || { echo "mk_vmsprocess_shr: DECC\$SHR.EXE not found: $DECC_SHR"; exit 1; }
[ -d "$SRC" ]      || { echo "mk_vmsprocess_shr: vmsprocess src dir not found: $SRC"; exit 1; }

WORK=${WORK:-/tmp/mk-vmsprocess-shr}
mkdir -p "$WORK"

CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics"
INCS="-I$SRC/include -I$LIBVMS_INC"

echo "mk_vmsprocess_shr: LINK.EXE=$LINK_EXE  CC=$CC  GSMATCH=$GSMATCH"
echo "mk_vmsprocess_shr: src=$SRC  DECC\$SHR=$DECC_SHR  LIBVMSSYS\$SHR=${LIBVMSSYS_SHR:-<none>}"

OBJS=""
for u in vms_pcb vms_process ast event_flags access_modes; do
    echo "  cc $u.c"
    $CC $CFLAGS $INCS -c -o "$WORK/$u.o" "$SRC/$u.c"
    OBJS="$OBJS $WORK/$u.o"
done

# The process-control universals LIBVMSPROCESS$SHR exports (every name is a
# non-static function defined in the 5 objects above). Each becomes a PROCEDURE
# universal in .vms$sv. Order is the append-only vector contract — DO NOT reorder
# or delete once consumers bind indices; only append (GSMATCH LEQUAL-compatible).
VEC="\
vms_pcb_get=PROCEDURE,vms_pcb_init=PROCEDURE,vms_pcb_cleanup=PROCEDURE,\
vms_pcb_set_identity=PROCEDURE,vms_pcb_set_default_dir=PROCEDURE,\
vms_get_current_process=PROCEDURE,vms_pid_from_linux=PROCEDURE,\
vms_format_uic=PROCEDURE,vms_parse_uic=PROCEDURE,\
eflag_init=PROCEDURE,eflag_set=PROCEDURE,eflag_clear=PROCEDURE,eflag_read=PROCEDURE,\
eflag_read_cluster=PROCEDURE,eflag_wait=PROCEDURE,eflag_wait_or=PROCEDURE,\
eflag_wait_and=PROCEDURE,\
ast_init=PROCEDURE,ast_cleanup=PROCEDURE,ast_queue=PROCEDURE,ast_set_enable=PROCEDURE,\
ast_deliver_pending=PROCEDURE,ast_is_enabled=PROCEDURE,ast_pending_count=PROCEDURE,\
access_mode_get=PROCEDURE,access_mode_set=PROCEDURE,access_mode_check=PROCEDURE,\
priv_check=PROCEDURE,priv_set=PROCEDURE,priv_get_all=PROCEDURE,priv_init=PROCEDURE"

USE="--use $DECC_SHR"
[ -n "$LIBVMSSYS_SHR" ] && [ -f "$LIBVMSSYS_SHR" ] && USE="$USE --use $LIBVMSSYS_SHR"

echo "mk_vmsprocess_shr: LINK.EXE --shareable $USE -> $OUT"
# STRICT (no --allow-undefined): every libc/pthread import MUST bind to DECC$SHR.
# shellcheck disable=SC2086
"$LINK_EXE" --shareable $USE \
    --symbol-vector "$VEC" \
    --gsmatch "$GSMATCH" \
    -o "$OUT" $OBJS

echo "mk_vmsprocess_shr: created $OUT"
