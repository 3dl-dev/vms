#!/bin/sh
# run_dcl_native.sh — the ENDPOINT test of the b65 VMS-native toolchain chain
# (bead vms-b65.6, pillar vms-ade): build DCL.EXE — the OVMX DCL shell — as a
# VMS-native EXECUTABLE image, activate it through IMGACT.EXE, and run a scripted
# DCL session (SHOW TIME / a symbol assignment) to VMS-correct output with a clean
# exit — NO ld / NO ld.so. This is S1 of the self-hosting milestone (vms-116) and
# the operator's canonical "all VMS" path.
#
# WHAT THIS SCRIPT PROVES (the vms-b65.6 endpoint — now GREEN, the toolchain that
# blocked it landed: vms-ba1 production emit_executable + vms-c86 executable TLS):
#   1. The full six-library producer graph links VMS-native, INCLUDING the 32 libc
#      universals appended to DECC$SHR for DCL (access..utimes) — the graph builds
#      STRICT-clean.
#   2. The real src/vmsdcl shell (22 TUs) + src/vmsqueue compile cleanly with the
#      proven musl VMS-native flags (-fPIC -ffreestanding -fno-builtin +
#      the one target-specific codegen flag -mno-outline-atomics on aarch64
#      / -mtls-dialect=gnu2 on x86_64 (vms-cb5f) + the DCL POSIX feature macros).
#   3. DCL.EXE links as a VMS-native ET_DYN executable via
#      `LINK.EXE --executable --use {DECC$SHR + the five OVMX shareables}` — a
#      23-object, main()-entered program with a full intra-image reloc set
#      (ADRP/ADD/ABS64/PREL32/LDST), its cross-image imports all bound, and its own
#      single-object TLS (dcl_messages.o) carried as PT_TLS.
#   4. IMGACT.EXE activates DCL.EXE (crt0 recovers argc/argv off the kernel stack,
#      calls main(argv[1]=session.com)); DCL runs a scripted session (SHOW TIME +
#      A = 2 + 3 / SHOW SYMBOL A) to VMS-correct output and exits clean. NO ld / NO
#      ld.so — the operator's canonical "all VMS" path. S1 of self-hosting (vms-116).
#
# DCL_EXPECT_LINK defaults to 1 (the toolchain is complete, so a link failure is a
# real regression = hard FAIL). link.c and imgact.c are the complete toolchain and
# are OUT of the Systems-Engineer file-domain — do NOT edit them here.
#
# ARCH selects the target (default aarch64; also x86_64, vms-cb5f — reproduces
# this same proof through the x86_64 LINK.EXE/IMGACT.EXE path merged by
# vms-8f5/vms-cd1/vms-2e4/vms-206). Run in a MATCHING musl container (arm64 or
# amd64 -- CLAUDE.md test loop): the container's native $CC drives both the
# object compiles and (per lib_build_graph.sh) IMGACT.EXE's own Makefile ARCH
# selection, and DCL.EXE activates directly (no qemu-user — the container IS
# the target arch). Needs root to create /vms.
set -e
ARCH=${ARCH:-aarch64}
CC=${CC:-gcc}
case "$ARCH" in
    aarch64) DCL_ARCHFLAG="-mno-outline-atomics" ;;
    x86_64)  DCL_ARCHFLAG="-mtls-dialect=gnu2" ;;
    *) echo "run_dcl_native: unsupported ARCH=$ARCH (expected aarch64 or x86_64)"; exit 1 ;;
esac
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
SRC=$(cd "$IMGACT_DIR/.." && pwd)            # src
LIBVMSSYS_DIR="$SRC/libvmssys"
VMSPROC_DIR="$SRC/vmsprocess"
VMSLNM_DIR="$SRC/vmslnm"
VMSFS_DIR="$SRC/vmsfs"
LIBVMS_DIR="$SRC/libvms"
VMSRMS_DIR="$SRC/vmsrms"
DCL_DIR="$SRC/vmsdcl"
LIBVMS_INC="$LIBVMS_DIR/include"
LNM_INC="$VMSLNM_DIR/include"
VMSFS_INC="$VMSFS_DIR/include"
RMS_INC="$VMSRMS_DIR/include"
WORK=${WORK:-/tmp/dcl-native}
rm -rf "$WORK"; mkdir -p "$WORK"

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need a $ARCH musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }

# Shared producer-graph build steps (vms-c39: factored out so LOGINOUT's
# sibling harness, run_login_native.sh, does not carry a second copy of
# "how do you build IMGACT.EXE, LINK.EXE and the six-library shareable graph
# in the arm64 musl container" -- see lib_build_graph.sh's header).
. "$HERE/lib_build_graph.sh"
build_producer_graph
echo "-- (DCL's own +32 DECC\$SHR universals were part of that graph build) --"

echo
echo "== compile the real src/vmsdcl (22 TUs) + src/vmsqueue VMS-native ($ARCH) =="
CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -U_FORTIFY_SOURCE $DCL_ARCHFLAG"
DEFS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"
# -I$LIBVMSSYS_DIR (vms-8019): dcl_cmd_show.c includes "vms_kif.h" — SHOW SYSTEM
# reads the executive's process table through vms_kif_procscan().
# -I$SRC/vmslink/include (vms-ca9): dcl_library.c includes "ovmx_olb.h" for the
# LIBRARY/OBJECT (.OLB) ar-container path.
INCS="-I$DCL_DIR/include -I$LIBVMS_INC -I$VMSFS_INC -I$LNM_INC -I$RMS_INC -I$VMSPROC_DIR/include -I$SRC/vmsqueue -I$LIBVMSSYS_DIR -I$SRC/vmsscs/include -I$SRC/vmslink/include"
TUS="dcl_main dcl_lexer dcl_parser dcl_exec dcl_backup dcl_builtin dcl_cmd_show \
dcl_cmd_set dcl_cmd_file dcl_cmd_process dcl_cmd_io dcl_cmd_misc dcl_disk_logical \
dcl_editor dcl_terminal dcl_symbol dcl_lexical dcl_filespec dcl_io dcl_script \
dcl_messages dcl_library dcl_help dcl_mbx"
DCLOBJS=""
for t in $TUS; do
    $CC $CFLAGS $DEFS $INCS -c -o "$WORK/$t.o" "$DCL_DIR/$t.c"
    DCLOBJS="$DCLOBJS $WORK/$t.o"
done
$CC $CFLAGS $DEFS $INCS -c -o "$WORK/vmsqueue.o" "$SRC/vmsqueue/vmsqueue.c"
DCLOBJS="$DCLOBJS $WORK/vmsqueue.o"
NDCL=$(echo $DCLOBJS | wc -w)
echo "-- $NDCL DCL (vmsdcl+vmsqueue) objects compiled VMS-native-clean --"
# 24 vmsdcl TUs + vmsqueue = 25 (dcl_disk_logical added by vms-f83;
# dcl_help added by vms-01b — the hierarchical HELP engine;
# dcl_mbx added by vms-786 — mailbox-backed SYS$INPUT/SYS$OUTPUT).
[ "$NDCL" -eq 25 ] || { echo "FAIL: expected 25 DCL objects, got $NDCL"; exit 1; }

# ODS-2 SYS$DISK adapter closure (epic vms-5eb, the ODS-2 runtime flip): the
# six objects mk_dcl.sh now links DIRECTLY into DCL.EXE as INTERNAL objects so
# DCL's DIRECTORY / SET DEFAULT / generic-filespec reroutes (rungs R3/vms-496 +
# R2/vms-af7a) can call ods2_sysdisk_* + ods2_fh2_parse. Compiled here too so
# the reloc/TLS diagnostic below reflects the real DCL.EXE object set. They are
# a DISTINCT module NOT part of LIBVMSFS$SHR's strict symbol vector; DCL.EXE (an
# executable) absorbs them as intra-image CALL26 targets. Present-but-uncalled
# until the reroute branches land — additive. See docs/design-ods2-runtime-flip.md.
ODS2_ADAPTER="vmsfs/vmsfs_volume vmsfs/ods2_sysdisk vmsfs/ods2/ods2_reader \
vmsfs/ods2/ods2_writer vmsfs/ods2/ods2_bdev vmsfs/ods2/ods2_path"
for o in $ODS2_ADAPTER; do
    b=$(basename "$o")
    $CC $CFLAGS $DEFS -I"$VMSFS_INC" -I"$LIBVMS_INC" -c -o "$WORK/$b.o" "$SRC/$o.c"
    DCLOBJS="$DCLOBJS $WORK/$b.o"
done
NOBJ=$(echo $DCLOBJS | wc -w)
echo "-- $NOBJ total DCL.EXE objects (25 DCL + 6 ODS-2 adapter) --"
# 25 DCL (vmsdcl+vmsqueue) + 6 ODS-2 SYS$DISK adapter (vmsfs_volume, ods2_sysdisk,
# ods2_reader/writer/bdev/path) = 31. Mirrors mk_dcl.sh's object set exactly.
[ "$NOBJ" -eq 31 ] || { echo "FAIL: expected 31 DCL.EXE objects, got $NOBJ"; exit 1; }

echo
echo "== DCL.EXE reloc/TLS profile (informative) =="
echo "-- entry symbol (crt0 synthesizes _start -> main): --"
nm $DCLOBJS 2>/dev/null | awk '$2=="T" && ($3=="main"||$3=="_start"){print "   "$3}'
echo "-- reloc histogram (intra-image + GOT + TLSDESC, per docs/design-link-x86_64-relocs.md on x86_64): --"
RELOC_PREFIX="R_AARCH64"; [ "$ARCH" = "x86_64" ] && RELOC_PREFIX="R_X86_64"
for o in $DCLOBJS; do readelf -rW "$o" 2>/dev/null; done \
  | awk -v p="$RELOC_PREFIX" '$0 ~ p{print $3}' | sort | uniq -c | sort -rn | sed 's/^/   /'
echo "-- TLS-defining objects (single-object TLS, within the per-image limit): --"
for o in $DCLOBJS; do readelf -SW "$o" 2>/dev/null | grep -qE '\.tdata|\.tbss' && echo "   TLS: $(basename $o)"; done

echo
echo "== link DCL.EXE VMS-native (LINK.EXE --executable --use the seven shareables) =="
set +e
CFLAGS="$CFLAGS" sh "$LINK_DIR/mk_dcl.sh" "$WORK/LINK.EXE" "$SYSEXE/DCL.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SYSLIB/LIBVMSSYS\$SHR.EXE" \
    "$DCL_DIR" "$SRC" 2>"$WORK/link.err"
LRC=$?
set -e
echo "-- LINK.EXE --executable exit=$LRC; message: --"
tail -3 "$WORK/link.err" | sed 's/^/   /'

if [ "$LRC" -ne 0 ]; then
    if [ "${DCL_EXPECT_LINK:-1}" = "1" ]; then
        echo "FAIL: DCL.EXE executable link failed (regression — the emit_executable"
        echo "  toolchain vms-ba1/vms-c86 is complete; DCL must link). See link.err above."
        exit 1
    fi
    echo "SKIP (DCL_EXPECT_LINK=0): DCL.EXE link failed but the assertion is disabled."
    exit 2
fi
readelf -lW "$SYSEXE/DCL.EXE" | grep -q 'INTERP' || { echo "FAIL: DCL.EXE has no PT_INTERP (IMGACT)"; exit 1; }

echo
echo "== DCL.EXE linked — activate through IMGACT.EXE + run a scripted session =="
mkdir -p /vms/SYS0/SYSCOMMON/SYSMGR
printf 'SHOW TIME\nA = 2 + 3\nSHOW SYMBOL A\nEXIT\n' > "$WORK/session.com"
set +e
"$SYSEXE/DCL.EXE" "$WORK/session.com" > "$WORK/session.out" 2>&1
RC=$?
set -e
echo "-- session output: --"; sed 's/^/   /' "$WORK/session.out"
echo "exit code = $RC"
grep -q 'A = 5' "$WORK/session.out" || { echo "FAIL: DCL did not evaluate A = 2 + 3 -> 5"; exit 1; }
[ "$RC" -eq 0 ] || { echo "FAIL: DCL scripted session did not exit clean (got $RC)"; exit 1; }

echo
echo "MILESTONE (vms-b65.6): the REAL src/vmsdcl DCL shell links VMS-native into"
echo "DCL.EXE (--use the six OVMX shareables), activates through IMGACT.EXE with NO"
echo "ld / NO ld.so, and runs a scripted DCL session to VMS-correct output. S1 of"
echo "self-hosting (vms-116)."
