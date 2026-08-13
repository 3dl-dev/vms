#!/bin/sh
# mk_dcl.sh — build recipe for DCL.EXE, the OVMX DCL shell as a VMS-native
# EXECUTABLE image (bead vms-b65.6, pillar vms-ade). The ENDPOINT of the b65
# lib-migration chain: the six OVMX libraries are now VMS-native shareables
# (DECC$SHR + LIBVMSPROCESS$SHR + LIBVMSLNM$SHR + LIBVMSFS$SHR + LIBVMS$SHR +
# LIBVMSRMS$SHR); DCL is the first real EXECUTABLE consumer that links against the
# whole graph and is activated by IMGACT.EXE — the operator's canonical "all VMS"
# path (NO ld / NO ld.so), S1 of the self-hosting milestone (vms-116).
#
# DCL.EXE is the src/vmsdcl shell (22 TUs) + src/vmsqueue (1 TU, the queue helper
# it statically absorbs). It is an ET_DYN EXECUTABLE (PT_INTERP=IMGACT.EXE, entry
# via the C runtime) that:
#   (a) has NO symbol vector (it exports nothing — it is the program, not a library);
#   (b) imports its externals, bound at activation via .vms$imp across the producers
#       (LINK.EXE prints the count for each build; it moves whenever DCL gains a call):
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
#           vms_severity_char/vms_strerror, AND the DATA table vms_months)
#                                                                       -> LIBVMS$SHR.
#           (vms_device_table / vms_device_count, the per-process MOUNT
#            facade, were DELETED by vms-651 along with the facade itself --
#            MOUNT/DISMOUNT now go through mount(2)/umount(2) and the
#            executive's device table, not a struct in this image.)
#       IMGACT pulls the full producer graph TRANSITIVELY from DCL's .vms$imp.
#   (c) is itself a single-TLS-object image: dcl_messages.o defines the __thread
#       message buffer (10 TLSDESC relocs) — within emit_shareable's supported
#       "one TLS object per image" limit; DCL.EXE therefore carries its own PT_TLS.
#
# Composition: the 22 vmsdcl translation units (== src/vmsdcl/CMakeLists.txt, minus
# readline which is a Docker-only convenience) + src/vmsqueue/vmsqueue.c. Compiled
# -fPIC musl with the proven lib-shareable flags PLUS the DCL POSIX feature macros:
#   -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics
#   -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
#   -fno-builtin        : keep mem*/str* as real CALL26 imports to DECC$SHR
#   -mno-outline-atomics: no __aarch64_* outline-atomic helpers DECC$SHR lacks
#
# ===================== TOOLCHAIN (vms-b65.6 — NOW GREEN) =====================
# DCL.EXE links with the production LINK.EXE. `emit_executable` (link.c) was rebuilt
# on the emit_shareable multi-object machinery (vms-ba1) + executable-own TLS
# (vms-c86): it links a 23-object program entered at main() whose relocs are
# dominated by INTRA-image references (2111 ADRP + 2120 ADD_ABS_LO12 + 981 ABS64 +
# 235 PREL32 + LDST* + 40 TLSDESC), synthesizes a crt0 (_start -> musl init -> main
# -> exit), binds 140 cross-image imports across the seven --use producers, and carries
# DCL's own single-TLS-object (dcl_messages.o) as PT_TLS. IMGACT.EXE recovers
# argc/argv off the kernel process stack and activates it. Empirical LINK result:
# ET_DYN executable, 22 objects, 8647 relocs, 5 GOT, 1 TLS, 1014 ABS64-ptr, 140
# imports. (These figures are a MEASUREMENT of one build, not a pin -- nothing
# asserts them, and any change to what DCL calls moves them. vms-8019 round 7
# moved the import count by one: SHOW SYSTEM's CPU column is now read from
# sys$getjpi instead of from a second /proc parser inside the DCL layer.) (link.c/imgact.c are the complete toolchain and are OUT of the
# Systems-Engineer file-domain; do NOT edit them here.)
# =============================================================================
#
# LIBVMSSYS$SHR$ (the SEVENTH producer) was added for vms-8019: SHOW SYSTEM is
# now a READER of the executive's process table, so dcl_cmd_show.c calls
# vms_kif_procscan() — a /dev/vms client entry point DEFINED in src/libvmssys,
# hence exported by LIBVMSSYS$SHR and by nothing else. DCL previously imported
# no vms_kif_* at all, which is exactly why the shell could only ever print the
# calling process: it had no way to ask the executive anything.
#
# Usage:  mk_dcl.sh <LINK.EXE> <out-DCL.EXE> \
#             <DECC$SHR.EXE> <LIBVMS$SHR.EXE> <LIBVMSPROCESS$SHR.EXE> \
#             <LIBVMSFS$SHR.EXE> <LIBVMSLNM$SHR.EXE> <LIBVMSRMS$SHR.EXE> \
#             <LIBVMSSYS$SHR.EXE> [vmsdcl-src-dir] [repo-src-dir]
# Env:    CC (default gcc)
# Must run in the arm64 musl container where the producer .EXE already exist.
# CC/CFLAGS are env-overridable (default aarch64 musl flags); the x86_64
# caller (vms-cb5f) sets CC + CFLAGS (with -mtls-dialect=gnu2, no
# -mno-outline-atomics) before invoking this script.
set -e

LINK_EXE=${1:?usage: mk_dcl.sh <LINK.EXE> <out> <DECC$SHR> <LIBVMS$SHR> <LIBVMSPROCESS$SHR> <LIBVMSFS$SHR> <LIBVMSLNM$SHR> <LIBVMSRMS$SHR> <LIBVMSSYS$SHR> [dcl-src] [repo-src]}
OUT=${2:?need output DCL.EXE path}
DECC_SHR=${3:?need DECC\$SHR.EXE}
VMS_SHR=${4:?need LIBVMS\$SHR.EXE}
PROC_SHR=${5:?need LIBVMSPROCESS\$SHR.EXE}
FS_SHR=${6:?need LIBVMSFS\$SHR.EXE}
LNM_SHR=${7:?need LIBVMSLNM\$SHR.EXE}
RMS_SHR=${8:?need LIBVMSRMS\$SHR.EXE}
SYS_SHR=${9:?need LIBVMSSYS\$SHR.EXE (vms_kif_* producer -- SHOW SYSTEM reads the executive process table)}
HERE=$(cd "$(dirname "$0")" && pwd)                      # src/vmslink
DCL=${10:-$(cd "$HERE/../vmsdcl" && pwd)}                # src/vmsdcl
REPO_SRC=${11:-$(cd "$HERE/.." && pwd)}                  # src
CC=${CC:-gcc}

for f in "$DECC_SHR" "$VMS_SHR" "$PROC_SHR" "$FS_SHR" "$LNM_SHR" "$RMS_SHR" "$SYS_SHR"; do
    [ -f "$f" ] || { echo "mk_dcl: producer image not found: $f"; exit 1; }
done
[ -d "$DCL" ] || { echo "mk_dcl: vmsdcl src dir not found: $DCL"; exit 1; }

WORK=${WORK:-/tmp/mk-dcl}
mkdir -p "$WORK"

CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics -U_FORTIFY_SOURCE}"
DEFS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"
INCS="-I$DCL/include -I$REPO_SRC/libvms/include -I$REPO_SRC/vmsfs/include \
-I$REPO_SRC/vmslnm/include -I$REPO_SRC/vmsrms/include \
-I$REPO_SRC/vmsprocess/include -I$REPO_SRC/vmsqueue -I$REPO_SRC/libvmssys \
-I$REPO_SRC/vmsscs/include \
-I$REPO_SRC/vmslink/include"   # scs_membership.h (SHOW CLUSTER, vms-8d4); ovmx_olb.h (LIBRARY/OBJECT, vms-ca9)

# The vmsdcl TUs (== src/vmsdcl/CMakeLists.txt, minus readline convenience).
# dcl_mbx added by vms-786 (mailbox-backed SYS$INPUT/SYS$OUTPUT).
TUS="dcl_main dcl_lexer dcl_parser dcl_exec dcl_backup dcl_builtin dcl_cmd_show \
dcl_cmd_set dcl_cmd_file dcl_cmd_process dcl_cmd_io dcl_cmd_misc dcl_disk_logical \
dcl_editor dcl_terminal dcl_symbol dcl_lexical dcl_filespec dcl_io dcl_script \
dcl_messages dcl_library dcl_help dcl_mbx"

echo "mk_dcl: LINK.EXE=$LINK_EXE  CC=$CC"
echo "mk_dcl: --use DECC\$SHR LIBVMS\$SHR LIBVMSPROCESS\$SHR LIBVMSFS\$SHR LIBVMSLNM\$SHR LIBVMSRMS\$SHR LIBVMSSYS\$SHR"

OBJS=""
for t in $TUS; do
    echo "  cc $t.c"
    $CC $CFLAGS $DEFS $INCS -c -o "$WORK/$t.o" "$DCL/$t.c"
    OBJS="$OBJS $WORK/$t.o"
done
echo "  cc vmsqueue.c"
$CC $CFLAGS $DEFS $INCS -c -o "$WORK/vmsqueue.o" "$REPO_SRC/vmsqueue/vmsqueue.c"
OBJS="$OBJS $WORK/vmsqueue.o"

echo "mk_dcl: LINK.EXE --executable --use {7 producers} -> $OUT"
# shellcheck disable=SC2086
"$LINK_EXE" --executable \
    --use "$DECC_SHR" --use "$VMS_SHR" --use "$PROC_SHR" \
    --use "$FS_SHR" --use "$LNM_SHR" --use "$RMS_SHR" --use "$SYS_SHR" \
    -o "$OUT" $OBJS

echo "mk_dcl: created $OUT"
