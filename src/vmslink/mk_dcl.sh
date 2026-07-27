#!/bin/sh
# mk_dcl.sh — build recipe for DCL.EXE, the OVMX DCL shell as a VMS-native
# EXECUTABLE image (bead vms-b65.6, pillar vms-ade). The ENDPOINT of the b65
# lib-migration chain: the six OVMX libraries are now VMS-native shareables
# (DECC$SHR + LIBVMSPROCESS$SHR + LIBVMSLNM$SHR + LIBVMSFS$SHR + LIBVMS$SHR +
# LIBVMSRMS$SHR); DCL is the first real EXECUTABLE consumer that links against the
# whole graph and is activated by IMGACT.EXE — the operator's canonical "all VMS"
# path (NO ld / NO ld.so), S1 of the self-hosting milestone (vms-116).
#
# DCL.EXE is the src/vmsdcl shell (21 TUs) + src/vmsqueue (1 TU, the queue helper
# it statically absorbs). It is an ET_DYN EXECUTABLE (PT_INTERP=IMGACT.EXE, entry
# via the C runtime) that:
#   (a) has NO symbol vector (it exports nothing — it is the program, not a library);
#   (b) imports 142 externals, bound at activation via .vms$imp across the producers:
#         - 114 libc/POSIX CALL + DATA imports (malloc/mem*/str*/printf/the stdio
#           FILE ops, fork/execvp/waitpid/system, the terminal ioctl/tc[gs]etattr/
#           isatty, pty pipe, inet_*/ntohl/socket, *rlimit, time/gettimeofday/
#           settimeofday/mktime/strptime/utimes, getpwnam/getpwuid, stdin/stdout/
#           stderr)                                                    -> DECC$SHR
#           (the 32 not previously pulled by any library consumer — access..utimes
#            — were APPENDED to DECC$SHR's vector for this bead; see mk_decc_shr.sh);
#         - lnm_* logical-name universals                              -> LIBVMSLNM$SHR;
#         - vms_pcb_* process-context universals                       -> LIBVMSPROCESS$SHR;
#         - vmsfs_* filespec/protection/device universals              -> LIBVMSFS$SHR;
#         - the libvms runtime universals (sys$sndopr, str_upcase_copy,
#           ovmx_accounting_get_lastlogin, vms_terminal_*/vms_term_*,
#           vms_severity_char/vms_strerror, AND the DATA tables vms_months /
#           vms_device_table / vms_device_count)                       -> LIBVMS$SHR.
#       IMGACT pulls the full producer graph TRANSITIVELY from DCL's .vms$imp.
#   (c) is itself a single-TLS-object image: dcl_messages.o defines the __thread
#       message buffer (10 TLSDESC relocs) — within emit_shareable's supported
#       "one TLS object per image" limit; DCL.EXE therefore carries its own PT_TLS.
#
# Composition: the 21 vmsdcl translation units (== src/vmsdcl/CMakeLists.txt, minus
# readline which is a Docker-only convenience) + src/vmsqueue/vmsqueue.c. Compiled
# -fPIC musl with the proven lib-shareable flags PLUS the DCL POSIX feature macros:
#   -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics
#   -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
#   -fno-builtin        : keep mem*/str* as real CALL26 imports to DECC$SHR
#   -mno-outline-atomics: no __aarch64_* outline-atomic helpers DECC$SHR lacks
#
# ============================ TOOLCHAIN GAP (vms-b65.6) =======================
# DCL.EXE does NOT link with the CURRENT LINK.EXE. `emit_executable` (link.c) is a
# single-object, _start-only, external-CALL26/JUMP26+GOT-only path — it loads only
# ins[0] ("single-object executable for now", link.c:1768), requires a _start symbol
# (link.c:596), and rejects every intra-image reloc and every non-CALL/GOT type
# (link.c:611/613). DCL is a 22-object program entered at main() whose relocs are
# dominated by INTRA-image references (2111 ADRP + 2120 ADD_ABS_LO12 + 981 ABS64 +
# 235 PREL32 + LDST*), i.e. exactly what the emit_SHAREABLE path already handles but
# emit_EXECUTABLE does not. This recipe is therefore READY-BUT-BLOCKED: it is the
# correct invocation for when the multi-object executable path lands. See the bead's
# escalation for the full empirically-measured requirement set. (link.c is out of the
# Systems-Engineer file-domain; do NOT edit it here.) NOTE also: R_AARCH64_PREL32
# (235 in DCL) is NOT handled by patch_pcrel today (link.c:811 default die) — it
# breaks even emit_shareable, so it must be added regardless.
# =============================================================================
#
# Usage:  mk_dcl.sh <LINK.EXE> <out-DCL.EXE> \
#             <DECC$SHR.EXE> <LIBVMS$SHR.EXE> <LIBVMSPROCESS$SHR.EXE> \
#             <LIBVMSFS$SHR.EXE> <LIBVMSLNM$SHR.EXE> <LIBVMSRMS$SHR.EXE> \
#             [vmsdcl-src-dir] [repo-src-dir]
# Env:    CC (default gcc)
# Must run in the arm64 musl container where the producer .EXE already exist.
set -e

LINK_EXE=${1:?usage: mk_dcl.sh <LINK.EXE> <out> <DECC$SHR> <LIBVMS$SHR> <LIBVMSPROCESS$SHR> <LIBVMSFS$SHR> <LIBVMSLNM$SHR> <LIBVMSRMS$SHR> [dcl-src] [repo-src]}
OUT=${2:?need output DCL.EXE path}
DECC_SHR=${3:?need DECC\$SHR.EXE}
VMS_SHR=${4:?need LIBVMS\$SHR.EXE}
PROC_SHR=${5:?need LIBVMSPROCESS\$SHR.EXE}
FS_SHR=${6:?need LIBVMSFS\$SHR.EXE}
LNM_SHR=${7:?need LIBVMSLNM\$SHR.EXE}
RMS_SHR=${8:?need LIBVMSRMS\$SHR.EXE}
HERE=$(cd "$(dirname "$0")" && pwd)                      # src/vmslink
DCL=${9:-$(cd "$HERE/../vmsdcl" && pwd)}                 # src/vmsdcl
REPO_SRC=${10:-$(cd "$HERE/.." && pwd)}                  # src
CC=${CC:-gcc}

for f in "$DECC_SHR" "$VMS_SHR" "$PROC_SHR" "$FS_SHR" "$LNM_SHR" "$RMS_SHR"; do
    [ -f "$f" ] || { echo "mk_dcl: producer image not found: $f"; exit 1; }
done
[ -d "$DCL" ] || { echo "mk_dcl: vmsdcl src dir not found: $DCL"; exit 1; }

WORK=${WORK:-/tmp/mk-dcl}
mkdir -p "$WORK"

CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics"
DEFS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"
INCS="-I$DCL/include -I$REPO_SRC/libvms/include -I$REPO_SRC/vmsfs/include \
-I$REPO_SRC/vmslnm/include -I$REPO_SRC/vmsrms/include \
-I$REPO_SRC/vmsprocess/include -I$REPO_SRC/vmsqueue"

# The 21 vmsdcl TUs (== src/vmsdcl/CMakeLists.txt, minus readline convenience).
TUS="dcl_main dcl_lexer dcl_parser dcl_exec dcl_backup dcl_builtin dcl_cmd_show \
dcl_cmd_set dcl_cmd_file dcl_cmd_process dcl_cmd_io dcl_cmd_misc dcl_editor \
dcl_terminal dcl_symbol dcl_lexical dcl_filespec dcl_io dcl_script dcl_messages \
dcl_library"

echo "mk_dcl: LINK.EXE=$LINK_EXE  CC=$CC"
echo "mk_dcl: --use DECC\$SHR LIBVMS\$SHR LIBVMSPROCESS\$SHR LIBVMSFS\$SHR LIBVMSLNM\$SHR LIBVMSRMS\$SHR"

OBJS=""
for t in $TUS; do
    echo "  cc $t.c"
    $CC $CFLAGS $DEFS $INCS -c -o "$WORK/$t.o" "$DCL/$t.c"
    OBJS="$OBJS $WORK/$t.o"
done
echo "  cc vmsqueue.c"
$CC $CFLAGS $DEFS $INCS -c -o "$WORK/vmsqueue.o" "$REPO_SRC/vmsqueue/vmsqueue.c"
OBJS="$OBJS $WORK/vmsqueue.o"

echo "mk_dcl: LINK.EXE --executable --use {6 producers} -> $OUT"
# shellcheck disable=SC2086
"$LINK_EXE" --executable \
    --use "$DECC_SHR" --use "$VMS_SHR" --use "$PROC_SHR" \
    --use "$FS_SHR" --use "$LNM_SHR" --use "$RMS_SHR" \
    -o "$OUT" $OBJS

echo "mk_dcl: created $OUT"
