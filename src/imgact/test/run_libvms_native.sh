#!/bin/sh
# run_libvms_native.sh — the FIFTH and LARGEST real OVMX-library migration onto the
# VMS-native toolchain (bead vms-b65.2, pillar vms-ade). Proves that the actual
# src/libvms library — the whole VMS runtime: system services ($ASSIGN/$QIO/$ENQ/...
# in syssvc/) + the lib$/str$/mth$/ots$ RTL — links VMS-native into LIBVMS$SHR.EXE,
# activates through IMGACT.EXE, and a consumer that imports one of its universals
# gets the VMS-correct result — with NO ld / NO ld.so.
#
# libvms is the FIRST b65 lib to depend on FOUR OVMX producers at once:
#   - libc/libm CALL imports + stdin/stdout/stderr DATA imports        -> DECC$SHR
#       (the libm transcendentals + fork/exec*/mmap/glob/time*/timer_* + the
#        stdin/stdout/stderr `FILE *const` DATA universals were APPENDED to
#        DECC$SHR's vector for this migration, mk_decc_shr.sh, append-only);
#   - vms_pcb_get/init/set_identity/set_default_dir                     -> LIBVMSPROCESS$SHR;
#   - vms_kif_open/enq/deq/convert (the /dev/vms lock-manager ioctls)   -> LIBVMSSYS$SHR
#       (its production vector is EXTENDED here from {vms_strlen} to also export the
#        four vms_kif_* universals — append-only, vms_strlen keeps index 0);
#   - vmsfs_to_linux_path (VMS filespec -> Linux path)                  -> LIBVMSFS$SHR.
# libvms does NOT import lnm_* directly, so LIBVMS$SHR does not --use LIBVMSLNM$SHR;
# but LIBVMSFS$SHR does, so IMGACT loads LIBVMSLNM$SHR TRANSITIVELY at activation
# (a 2-level transitive chain: consumer -> libvms -> libvmsfs -> libvmslnm). All
# five producer images live in SYS$SHARE so IMGACT can resolve the whole graph.
# LIBVMS$SHR is ALSO a TLS producer (rtl/lib_signal.c's one __thread object), so the
# activation loads TWO TLS producers (LIBVMSPROCESS$SHR + LIBVMS$SHR); IMGACT's
# assign_tls_offsets gives each module its own TP-relative offset.
#
# Chain, all VMS-native (no ld / no ld.so):
#   1. build IMGACT.EXE + LINK.EXE.
#   2. mk_decc_shr.sh whole-archives musl libc.a + libgcc.a into DECC$SHR.EXE.
#   3. LIBVMSSYS$SHR.EXE: real src/libvmssys, vector {vms_strlen + vms_kif_*}.
#   4. mk_vmsprocess_shr.sh -> LIBVMSPROCESS$SHR.EXE (vms_pcb_* producer, TLS).
#   5. mk_vmslnm_shr.sh -> LIBVMSLNM$SHR.EXE   (transitive dep of vmsfs).
#   6. mk_vmsfs_shr.sh  -> LIBVMSFS$SHR.EXE    (vmsfs_to_linux_path producer).
#   7. mk_libvms_shr.sh compiles the 40 real libvms objects and links LIBVMS$SHR.EXE
#      via LINK.EXE --shareable --use {DECC$SHR,LIBVMSPROCESS$SHR,LIBVMSSYS$SHR,
#      LIBVMSFS$SHR}: EXPORTS its universals, IMPORTS libc/libm+DATA from DECC$SHR,
#      vms_pcb_* from LIBVMSPROCESS$SHR, vms_kif_* from LIBVMSSYS$SHR,
#      vmsfs_to_linux_path from LIBVMSFS$SHR. STRICT link: every import MUST bind.
#   8. LINK.EXE --executable --use LIBVMS$SHR builds a consumer that imports ONLY one
#      libvms universal: lib$extzv (the RTL zero-extended bit-field extract). It
#      never names any producer, so ALL binds are purely TRANSITIVE.
#   9. RUN the consumer FOR REAL: kernel -> IMGACT.EXE -> load LIBVMS$SHR ->
#      (transitively) load DECC$SHR + LIBVMSPROCESS$SHR + LIBVMSSYS$SHR + LIBVMSFS$SHR
#      (-> LIBVMSLNM$SHR), bind every libvms import, bind the consumer's lib$extzv
#      import, drive musl __init_libc, absorb BOTH TLS modules against musl's TP ->
#      transfer control. The consumer:
#        base = { 0x2A }  (one byte = 42); lib$extzv(pos=0, size=8, base) -> 42.
#      lib$extzv extracts the zero-extended 8-bit field at bit 0 == the byte value
#      42. Consumer exits 42 iff lib$extzv returned 42. Exit 42 proves: the libvms
#      RTL universal bound in the consumer, LIBVMS$SHR activated, its four producers
#      (a 2-level transitive graph incl. LIBVMSLNM$SHR) loaded and bound, both TLS
#      modules were absorbed against musl's TP, and the RTL computed the VMS-correct
#      bit-field result.
#
# Uses the arm64 musl container's libc.a + libgcc.a AND linux-headers (sys_uring.c
# includes <linux/io_uring.h>). aarch64-only for now (CLAUDE.md test loop). Needs
# root to create /vms. Exit 0 on ok.

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
LIBVMS_INC=$(cd "$LIBVMS_DIR/include" && pwd)
LNM_INC=$(cd "$VMSLNM_DIR/include" && pwd)
WORK=${WORK:-/tmp/libvms-native}
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

echo "== LIBVMSSYS\$SHR.EXE: real src/libvmssys, vector extended with vms_kif_* =="
# libvms imports vms_kif_open/enq/deq/convert from LIBVMSSYS$SHR. The b65.1
# milestone vector exported only vms_strlen; extend it (append-only, vms_strlen
# stays index 0) so libvms's lock-manager ioctl imports bind. Freestanding syscall
# layer: no --use (no producer deps).
#
# APPENDED for vms-8019 (append-only, so every prior consumer's bound vector
# index is unchanged — GSMATCH LEQUAL-compatible): the process-table client
# calls. $CREPRC/$GETJPI in src/libvms/syssvc/sys_process.c became READERS of
# the executive's process table, so libvms now cross-image-imports
# vms_kif_setprn + vms_kif_getjpi_{self,pid,prcnam}; DCL's SHOW SYSTEM
# enumerates the table, so DCL.EXE imports vms_kif_procscan (mk_dcl.sh --use's
# LIBVMSSYS$SHR for exactly this). THE RULE THIS ENCODES: every /dev/vms entry
# point a product library calls must be a universal of the image that DEFINES
# it, or the VMS-native link fails — LIBVMSSYS$SHR is the producer of the
# executive client, and nothing else may re-export it.
#
# APPENDED AGAIN for vms-2a8, same rule, same append-only discipline (every
# index above is unchanged): event flags became executive-resident. $SETEF /
# $CLREF / $READEF / $WAITFR / $WFLOR / $WFLAND / $ASCEFC / $DACEFC / $DLCEFC in
# src/libvms/syssvc/sys_event.c are now READERS AND WRITERS of the executive's
# flag clusters (src/kernel/vms_eflag.c) instead of a per-process PCB copy, so
# libvms cross-image-imports the nine matching vms_kif_* calls. This is the
# VMS-native link telling the truth about the wiring: before vms-2a8 sys_event.o
# named no vms_kif_* symbol at all, which is exactly what "the facility was a
# facade" looks like from the linker's side.
SYSCFLAGS="-fPIC -O2 -ffreestanding -fno-stack-protector -fno-builtin -mno-outline-atomics -I$LIBVMSSYS_DIR"
SYSOBJS=""
for c in vms_string vms_snprintf vms_futex vms_stdio vms_math vms_runtime_init vms_kif; do
    $CC $SYSCFLAGS -c -o "$WORK/sys_$c.o" "$LIBVMSSYS_DIR/$c.c"
    SYSOBJS="$SYSOBJS $WORK/sys_$c.o"
done
$CC -fPIC -mno-outline-atomics -c -o "$WORK/sys_syscall.o" "$LIBVMSSYS_DIR/arch/aarch64/syscall.S"
SYSOBJS="$SYSOBJS $WORK/sys_syscall.o"
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "vms_strlen=PROCEDURE,vms_kif_open=PROCEDURE,vms_kif_enq=PROCEDURE,vms_kif_deq=PROCEDURE,vms_kif_convert=PROCEDURE,vms_kif_assign=PROCEDURE,vms_kif_dassgn=PROCEDURE,vms_kif_getdvi_chan=PROCEDURE,vms_kif_setprn=PROCEDURE,vms_kif_getjpi_self=PROCEDURE,vms_kif_getjpi_pid=PROCEDURE,vms_kif_getjpi_prcnam=PROCEDURE,vms_kif_procscan=PROCEDURE,vms_kif_setef=PROCEDURE,vms_kif_clref=PROCEDURE,vms_kif_readef=PROCEDURE,vms_kif_waitfr=PROCEDURE,vms_kif_wflor=PROCEDURE,vms_kif_wfland=PROCEDURE,vms_kif_ascefc=PROCEDURE,vms_kif_dacefc=PROCEDURE,vms_kif_dlcefc=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$SYSLIB/LIBVMSSYS\$SHR.EXE" $SYSOBJS
readelf -SW "$SYSLIB/LIBVMSSYS\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: LIBVMSSYS\$SHR no symbol vector"; exit 1; }

echo "== mk_vmsprocess_shr.sh: real src/vmsprocess -> LIBVMSPROCESS\$SHR.EXE (vms_pcb_* producer) =="
CC="$CC" sh "$LINK_DIR/mk_vmsprocess_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "" "$VMSPROC_DIR" "$LIBVMS_INC"

echo "== mk_vmslnm_shr.sh: real src/vmslnm -> LIBVMSLNM\$SHR.EXE (transitive dep of vmsfs) =="
CC="$CC" sh "$LINK_DIR/mk_vmslnm_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$VMSLNM_DIR" "$LIBVMS_INC"

echo "== mk_vmsfs_shr.sh: real src/vmsfs -> LIBVMSFS\$SHR.EXE (vmsfs_to_linux_path producer) =="
CC="$CC" sh "$LINK_DIR/mk_vmsfs_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" \
    "$VMSFS_DIR" "$LIBVMS_INC" "$LNM_INC"

echo "== mk_libvms_shr.sh: real src/libvms -> LIBVMS\$SHR.EXE (THE main runtime) =="
# STRICT link inside the recipe (no --allow-undefined). An unresolved libc/libm
# symbol => DECC$SHR's vector needs it appended; unresolved vms_kif_* =>
# LIBVMSSYS$SHR's vector; vms_pcb_* => LIBVMSPROCESS$SHR; vmsfs_to_linux_path =>
# LIBVMSFS$SHR.
CC="$CC" sh "$LINK_DIR/mk_libvms_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSSYS\$SHR.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    "$LIBVMS_DIR"

echo "-- LIBVMS\$SHR.EXE: PT_TLS (lib_signal) + symbol vector + bound imports --"
readelf -lW "$SYSLIB/LIBVMS\$SHR.EXE" | grep -E '\bTLS\b' || true
readelf -SW "$SYSLIB/LIBVMS\$SHR.EXE" | grep -E '\.tbss|\.tdata|\.vms\$tls|\.vms\$sv|\.vms\$imp|\.plt|\.igot' || true
readelf -SW "$SYSLIB/LIBVMS\$SHR.EXE" | grep -q '\.vms\$sv'  || { echo "FAIL: no .vms\$sv (no universals)"; exit 1; }
readelf -SW "$SYSLIB/LIBVMS\$SHR.EXE" | grep -q '\.vms\$imp' || { echo "FAIL: no .vms\$imp (imports not bound)"; exit 1; }
readelf -lW "$SYSLIB/LIBVMS\$SHR.EXE" | grep -q '\bTLS\b'    || { echo "FAIL: LIBVMS\$SHR has no PT_TLS (lib_signal __thread expected)"; exit 1; }
readelf -SW "$SYSLIB/LIBVMS\$SHR.EXE" | grep -q '\.vms\$tls' || { echo "FAIL: LIBVMS\$SHR emitted no .vms\$tls"; exit 1; }

echo "== LINK.EXE --executable --use LIBVMS\$SHR -> consumer =="
# Imports ONLY the lib$extzv universal. Every producer bind is transitive (IMGACT
# must pull the whole graph from LIBVMS$SHR's .vms$imp). Strings/data built as
# immediate stores on the stack -> the object carries ONLY the CALL26 reloc for
# lib$extzv (no .rodata / ADRP relocs the VMS-native emit_executable rejects,
# vms-338). Built at -O0 so gcc does not coalesce the byte stores into a constant
# pool.
cat > "$WORK/cons.c" <<'EOF'
#include <stdint.h>
/* lib$extzv(pos, size, base): zero-extended bit-field extract from the VMS RTL.
 * pos/size are pointers to longword/byte; base points to the bit array. */
extern uint32_t lib$extzv(const int32_t *pos, const uint8_t *size,
                          const void *base);

void _start(void)
{
    int32_t pos = 0;                 /* bit 0 */
    uint8_t size = 8;                /* 8-bit field */
    unsigned char base[4];
    base[0] = 0x2A;                  /* 42 */
    base[1] = 0x00; base[2] = 0x00; base[3] = 0x00;

    uint32_t v = lib$extzv(&pos, &size, base);   /* == 42 */
    int code = (v == 42) ? 42 : 1;

    register long x8 __asm__("x8") = 94;   /* exit_group */
    register long x0 __asm__("x0") = code;
    __asm__ volatile("svc 0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
EOF
$CC -fPIC -O0 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics \
    -c -o "$WORK/cons.o" "$WORK/cons.c"
"$WORK/LINK.EXE" --executable --use "$SYSLIB/LIBVMS\$SHR.EXE" \
    -o "$WORK/CONS.EXE" "$WORK/cons.o"
chmod +x "$WORK/CONS.EXE"

echo
echo "== RUN ./CONS.EXE FOR REAL (kernel -> IMGACT -> LIBVMS\$SHR -> 4 producers) =="
set +e
"$WORK/CONS.EXE"; RC=$?
set -e
echo "exit code = $RC (expect 42 = lib\$extzv(pos=0,size=8,{0x2A}) == 42)"
[ "$RC" -eq 42 ] || { echo "FAIL: libvms VMS-native migration did not run correctly (got $RC, want 42)"; exit 1; }

echo
echo "MILESTONE (vms-b65.2): the REAL src/libvms runtime (system services + lib\$/"
echo "str\$/mth\$/ots\$ RTL, 40 objects) links VMS-native into LIBVMS\$SHR.EXE — its"
echo "libc/libm+DATA imports bound to DECC\$SHR, vms_pcb_* to LIBVMSPROCESS\$SHR,"
echo "vms_kif_* to LIBVMSSYS\$SHR, vmsfs_to_linux_path to LIBVMSFS\$SHR — activates"
echo "through IMGACT.EXE (2-level transitive graph, two TLS modules), and a consumer"
echo "gets the VMS-correct RTL result. The largest b65 link; unblocks vms-b65.5 (RMS)."
