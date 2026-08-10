#!/bin/sh
# run_vmsrms_native.sh — the SIXTH and LAST real OVMX-library migration onto the
# VMS-native toolchain before DCL (bead vms-b65.5, pillar vms-ade). Proves that the
# actual src/vmsrms library — VMS Record Management Services ($OPEN/$CREATE/$CONNECT/
# $GET/$PUT/$PARSE/$SEARCH over sequential/relative/indexed files, FAB/RAB/NAM/XAB)
# — links VMS-native into LIBVMSRMS$SHR.EXE, activates through IMGACT.EXE, and a
# consumer that imports one RMS universal gets the VMS-correct result — NO ld / NO
# ld.so.
#
# vmsrms sits on top of the WHOLE b65 producer graph. Its cross-image imports:
#   - libc CALL imports (malloc/calloc/free, mem*/str*/strtoul, snprintf, the stdio
#     FILE ops + POSIX file/dir ops, fnmatch, getenv/getuid/getgid,
#     __errno_location, pthread_mutex_lock/unlock)                     -> DECC$SHR
#     (fnmatch/fsync/ftruncate were APPENDED to DECC$SHR's vector for this migration
#      -- mk_decc_shr.sh, append-only after the DATA entries so existing consumers'
#      vector indices are unchanged; everything else vmsrms needs was already there);
#   - vms$check_access (libvms file-protection security service)        -> LIBVMS$SHR;
#   - vmsfs_parse_filespec/compose_filespec/to_linux_path/to_vms_spec/
#     get_highest_version/mode_to_protection/protection_to_mode         -> LIBVMSFS$SHR.
# vmsrms does NOT import lnm_*/vms_pcb_*/vms_kif_* directly, so LIBVMSRMS$SHR does
# not --use LIBVMSLNM$SHR/LIBVMSPROCESS$SHR/LIBVMSSYS$SHR — but LIBVMS$SHR and
# LIBVMSFS$SHR do, so IMGACT loads that whole graph TRANSITIVELY at activation. All
# producer images live in SYS$SHARE so IMGACT resolves the full chain. vmsrms defines
# NO __thread object -> LIBVMSRMS$SHR is NOT a TLS producer (no PT_TLS); the two TLS
# producers it transitively loads (LIBVMSPROCESS$SHR + LIBVMS$SHR) each get their own
# TP offset from IMGACT's assign_tls_offsets.
#
# Chain, all VMS-native (no ld / no ld.so):
#   1. build IMGACT.EXE + LINK.EXE.
#   2. mk_decc_shr.sh whole-archives musl libc.a + libgcc.a into DECC$SHR.EXE.
#   3. LIBVMSSYS$SHR.EXE: real src/libvmssys, vector {vms_strlen + vms_kif_*}.
#   4. mk_vmsprocess_shr.sh -> LIBVMSPROCESS$SHR.EXE (vms_pcb_* producer, TLS).
#   5. mk_vmslnm_shr.sh -> LIBVMSLNM$SHR.EXE   (transitive dep of vmsfs).
#   6. mk_vmsfs_shr.sh  -> LIBVMSFS$SHR.EXE    (vmsfs_* producer).
#   7. mk_libvms_shr.sh -> LIBVMS$SHR.EXE      (vms$check_access producer, TLS).
#   8. mk_vmsrms_shr.sh compiles the 8 real vmsrms objects and links LIBVMSRMS$SHR.EXE
#      via LINK.EXE --shareable --use {DECC$SHR,LIBVMS$SHR,LIBVMSFS$SHR}: EXPORTS its
#      RMS universals, IMPORTS libc from DECC$SHR, vms$check_access from LIBVMS$SHR,
#      vmsfs_* from LIBVMSFS$SHR. STRICT link: every import MUST bind.
#   9. LINK.EXE --executable --use LIBVMSRMS$SHR builds a consumer that imports ONLY
#      one RMS universal: sys$parse. It never names any producer, so ALL binds are
#      purely TRANSITIVE (IMGACT pulls the whole graph from LIBVMSRMS$SHR's .vms$imp).
#  10. RUN the consumer FOR REAL: kernel -> IMGACT.EXE -> load LIBVMSRMS$SHR ->
#      (transitively) load DECC$SHR + LIBVMS$SHR + LIBVMSFS$SHR (-> LIBVMSPROCESS$SHR
#      + LIBVMSSYS$SHR + LIBVMSLNM$SHR), bind every vmsrms import, bind the consumer's
#      sys$parse import, drive musl __init_libc, absorb the transitive TLS modules
#      against musl's TP -> transfer control. The consumer:
#        FAB.fna = "[MYDIR]DATA.TXT" (fns=15); NAM.esa = buf; sys$parse(&fab)
#          -> RMS$_NORMAL (65537), NAM.nam$b_name == 4 ("DATA"),
#             NAM.nam$b_type == 4 (".TXT").
#      $PARSE is the RMS filespec analyzer: it drives vmsfs_parse_filespec +
#      vmsfs_compose_filespec (LIBVMSFS$SHR) then walks the expanded string to split
#      it into device/dir/name/type/version components. Consumer exits 42 iff
#      (rc==RMS$_NORMAL && name-component==4 && type-component==4). Exit 42 proves the
#      RMS universal bound in the consumer, LIBVMSRMS$SHR activated, its transitive
#      producer graph (incl. LIBVMSFS$SHR + libvms + libc, two TLS modules) loaded
#      and bound, and RMS $PARSE computed the VMS-correct component breakdown.
#      Informative failure codes: 10 = rc != RMS$_NORMAL; 20+name on name mismatch;
#      40+type on type mismatch.
#
# Uses the arm64 musl container's libc.a + libgcc.a AND linux-headers (libvms's
# sys_uring.c includes <linux/io_uring.h>). aarch64-only for now (CLAUDE.md test
# loop). Needs root to create /vms. Exit 0 on ok.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
LIBVMSSYS_DIR=$(cd "$IMGACT_DIR/../libvmssys" && pwd)
VMSPROC_DIR=$(cd "$IMGACT_DIR/../vmsprocess" && pwd)
VMSLNM_DIR=$(cd "$IMGACT_DIR/../vmslnm" && pwd)
VMSFS_DIR=$(cd "$IMGACT_DIR/../vmsfs" && pwd)
LIBVMS_DIR=$(cd "$IMGACT_DIR/../libvms" && pwd)
VMSRMS_DIR=$(cd "$IMGACT_DIR/../vmsrms" && pwd)
LIBVMS_INC=$(cd "$LIBVMS_DIR/include" && pwd)
LNM_INC=$(cd "$VMSLNM_DIR/include" && pwd)
VMSFS_INC=$(cd "$VMSFS_DIR/include" && pwd)
RMS_INC=$(cd "$VMSRMS_DIR/include" && pwd)
WORK=${WORK:-/tmp/vmsrms-native}
rm -rf "$WORK"; mkdir -p "$WORK"

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need arm64 musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }

echo "== build IMGACT.EXE + LINK.EXE =="
( cd "$IMGACT_DIR" && make CC="$CC" clean >/dev/null 2>&1 || true; make CC="$CC" ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

echo "== mk_decc_shr.sh: whole-archive musl -> DECC\$SHR.EXE (production vector) =="
sh "$LINK_DIR/mk_decc_shr.sh" "$WORK/LINK.EXE" "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
readelf -SW "$SYSLIB/DECC\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: DECC\$SHR has no symbol vector"; exit 1; }

echo "== LIBVMSSYS\$SHR.EXE: real src/libvmssys, vector {vms_strlen + vms_kif_*} =="
# libvms (transitive dep) imports vms_kif_open/enq/deq/convert from LIBVMSSYS$SHR.
SYSCFLAGS="-fPIC -O2 -ffreestanding -fno-stack-protector -fno-builtin -mno-outline-atomics -I$LIBVMSSYS_DIR"
SYSOBJS=""
for c in vms_string vms_snprintf vms_futex vms_stdio vms_math vms_runtime_init vms_kif; do
    $CC $SYSCFLAGS -c -o "$WORK/sys_$c.o" "$LIBVMSSYS_DIR/$c.c"
    SYSOBJS="$SYSOBJS $WORK/sys_$c.o"
done
$CC -fPIC -mno-outline-atomics -c -o "$WORK/sys_syscall.o" "$LIBVMSSYS_DIR/arch/aarch64/syscall.S"
SYSOBJS="$SYSOBJS $WORK/sys_syscall.o"
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "vms_strlen=PROCEDURE,vms_kif_open=PROCEDURE,vms_kif_enq=PROCEDURE,vms_kif_deq=PROCEDURE,vms_kif_convert=PROCEDURE,vms_kif_assign=PROCEDURE,vms_kif_dassgn=PROCEDURE,vms_kif_getdvi_chan=PROCEDURE,vms_kif_getdvi_devnam=PROCEDURE,vms_kif_devscan=PROCEDURE,vms_kif_setprn=PROCEDURE,vms_kif_setprv=PROCEDURE,vms_kif_getjpi_self=PROCEDURE,vms_kif_getjpi_pid=PROCEDURE,vms_kif_getjpi_prcnam=PROCEDURE,vms_kif_procscan=PROCEDURE,vms_kif_setef=PROCEDURE,vms_kif_clref=PROCEDURE,vms_kif_readef=PROCEDURE,vms_kif_waitfr=PROCEDURE,vms_kif_wflor=PROCEDURE,vms_kif_wfland=PROCEDURE,vms_kif_ascefc=PROCEDURE,vms_kif_dacefc=PROCEDURE,vms_kif_dlcefc=PROCEDURE,vms_kif_dclast=PROCEDURE,vms_kif_setast=PROCEDURE,vms_kif_deliverast=PROCEDURE,vms_kif_lnm_define=PROCEDURE,vms_kif_lnm_delete=PROCEDURE,vms_kif_lnm_translate=PROCEDURE,vms_kif_mbx_create=PROCEDURE,vms_kif_mbx_assign=PROCEDURE,vms_kif_mbx_delmbx=PROCEDURE,vms_kif_mbx_write=PROCEDURE,vms_kif_mbx_read=PROCEDURE,vms_kif_p0_map=PROCEDURE,vms_kif_p0_unmap=PROCEDURE,vms_kif_p1_map=PROCEDURE,vms_kif_enter_image=PROCEDURE,vms_kif_image_rundown=PROCEDURE,vms_kif_p1_protect=PROCEDURE,vms_kif_lnm_enumerate=PROCEDURE,vms_kif_disk_resolve=PROCEDURE,vms_kif_chkpriv=PROCEDURE,vms_kif_alloc=PROCEDURE,vms_kif_dalloc=PROCEDURE,vms_kif_establish_system=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$SYSLIB/LIBVMSSYS\$SHR.EXE" $SYSOBJS

echo "== mk_vmsprocess_shr.sh: real src/vmsprocess -> LIBVMSPROCESS\$SHR.EXE =="
CC="$CC" sh "$LINK_DIR/mk_vmsprocess_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "" "$VMSPROC_DIR" "$LIBVMS_INC"

echo "== mk_vmslnm_shr.sh: real src/vmslnm -> LIBVMSLNM\$SHR.EXE =="
CC="$CC" sh "$LINK_DIR/mk_vmslnm_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$VMSLNM_DIR" "$LIBVMS_INC"

echo "== mk_vmsfs_shr.sh: real src/vmsfs -> LIBVMSFS\$SHR.EXE (vmsfs_* producer) =="
CC="$CC" sh "$LINK_DIR/mk_vmsfs_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" \
    "$VMSFS_DIR" "$LIBVMS_INC" "$LNM_INC"

echo "== mk_libvms_shr.sh: real src/libvms -> LIBVMS\$SHR.EXE (vms\$check_access producer) =="
CC="$CC" sh "$LINK_DIR/mk_libvms_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSSYS\$SHR.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    "$LIBVMS_DIR"

echo "== mk_vmsrms_shr.sh: real src/vmsrms -> LIBVMSRMS\$SHR.EXE (THE RMS layer) =="
# STRICT link inside the recipe (no --allow-undefined). An unresolved libc symbol =>
# DECC$SHR's vector needs it appended; unresolved vms$check_access => LIBVMS$SHR's
# vector; unresolved vmsfs_* => LIBVMSFS$SHR's vector.
CC="$CC" sh "$LINK_DIR/mk_vmsrms_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    "$VMSRMS_DIR" "$LIBVMS_INC" "$VMSFS_INC"

echo "-- LIBVMSRMS\$SHR.EXE: symbol vector + bound imports; NO TLS (vmsrms has no __thread) --"
readelf -SW "$SYSLIB/LIBVMSRMS\$SHR.EXE" | grep -E '\.vms\$sv|\.vms\$imp|\.plt|\.igot' || true
readelf -SW "$SYSLIB/LIBVMSRMS\$SHR.EXE" | grep -q '\.vms\$sv'  || { echo "FAIL: no .vms\$sv (no universals)"; exit 1; }
readelf -SW "$SYSLIB/LIBVMSRMS\$SHR.EXE" | grep -q '\.vms\$imp' || { echo "FAIL: no .vms\$imp (imports not bound)"; exit 1; }
# vmsrms is NOT a TLS producer: assert the negative so a future accidental __thread
# (which would silently need the multi-object-TLS path, vms-212) is caught here.
if readelf -lW "$SYSLIB/LIBVMSRMS\$SHR.EXE" | grep -q '\bTLS\b'; then
    echo "FAIL: LIBVMSRMS\$SHR unexpectedly has PT_TLS (vmsrms gained a __thread object?)"; exit 1
fi

echo "== LINK.EXE --executable --use LIBVMSRMS\$SHR -> consumer =="
# Imports ONLY the sys$parse universal; every producer bind is transitive (IMGACT
# pulls the whole graph from LIBVMSRMS$SHR's .vms$imp). The FAB/NAM control blocks
# live on the stack, zeroed by an explicit byte loop and filled with immediate
# stores; the filespec string is built byte-by-byte (string LITERALS would land in
# the consumer's .rodata reached via ADRP/ADD relocs, which VMS-native
# emit_executable rejects -- only CALL26/JUMP26 + GOT, vms-338). Built at -O0 so
# gcc does not coalesce the byte stores into a .rodata constant pool.
cat > "$WORK/cons.c" <<EOF
#include "rms/rms.h"

void _start(void)
{
    struct FAB fab;
    struct NAM nam;
    char spec[16];       /* "[MYDIR]DATA.TXT" */
    char esa[256];       /* expanded-string buffer for \$PARSE */

    /* zero the control blocks (memset would be an extra import; keep it inline) */
    { char *p = (char *)&fab; for (unsigned i = 0; i < sizeof(fab); i++) p[i] = 0; }
    { char *p = (char *)&nam; for (unsigned i = 0; i < sizeof(nam); i++) p[i] = 0; }

    spec[0]='['; spec[1]='M'; spec[2]='Y'; spec[3]='D'; spec[4]='I'; spec[5]='R';
    spec[6]=']'; spec[7]='D'; spec[8]='A'; spec[9]='T'; spec[10]='A'; spec[11]='.';
    spec[12]='T'; spec[13]='X'; spec[14]='T'; spec[15]=0;

    fab.fab\$b_bid = FAB\$C_BID;          /* 3 */
    fab.fab\$l_fna = spec;
    fab.fab\$b_fns = 15;
    fab.fab\$l_nam = &nam;

    nam.nam\$b_bid = NAM\$C_BID;          /* 4 */
    nam.nam\$l_esa = esa;
    nam.nam\$b_ess = 255;

    unsigned rc = sys\$parse(&fab);       /* RMS\$_NORMAL == 65537 */

    int code;
    if (rc != RMS\$_NORMAL)          code = 10;                 /* parse failed */
    else if (nam.nam\$b_name != 4)   code = 20 + nam.nam\$b_name; /* "DATA" -> 4 */
    else if (nam.nam\$b_type != 4)   code = 40 + nam.nam\$b_type; /* ".TXT" -> 4 */
    else                            code = 42;                 /* VMS-correct */

    register long x8 __asm__("x8") = 94;   /* exit_group */
    register long x0 __asm__("x0") = code;
    __asm__ volatile("svc 0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
EOF
$CC -fPIC -O0 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics \
    -I"$RMS_INC" -I"$LIBVMS_INC" \
    -c -o "$WORK/cons.o" "$WORK/cons.c"
"$WORK/LINK.EXE" --executable --use "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    -o "$WORK/CONS.EXE" "$WORK/cons.o"
chmod +x "$WORK/CONS.EXE"

echo
echo "== RUN ./CONS.EXE FOR REAL (kernel -> IMGACT -> LIBVMSRMS\$SHR -> whole graph) =="
set +e
"$WORK/CONS.EXE"; RC=$?
set -e
echo "exit code = $RC (expect 42 = sys\$parse==RMS\$_NORMAL && name==4 (DATA) && type==4 (.TXT))"
[ "$RC" -eq 42 ] || { echo "FAIL: vmsrms VMS-native migration did not run correctly (got $RC, want 42)"; exit 1; }

echo
echo "MILESTONE (vms-b65.5): the REAL src/vmsrms Record Management Services (8 objects:"
echo "\$OPEN/\$CREATE/\$CONNECT/\$GET/\$PUT/\$PARSE/\$SEARCH over seq/rel/idx files) links"
echo "VMS-native into LIBVMSRMS\$SHR.EXE — its libc imports bound to DECC\$SHR,"
echo "vms\$check_access to LIBVMS\$SHR, its filespec universals to LIBVMSFS\$SHR (no TLS)"
echo "— activates through IMGACT.EXE (full transitive graph, two TLS modules), and a"
echo "consumer gets the VMS-correct \$PARSE component breakdown. The LAST b65 library"
echo "before DCL; unblocks vms-b65.6 (DCL VMS-native, S1 of self-hosting vms-116)."
