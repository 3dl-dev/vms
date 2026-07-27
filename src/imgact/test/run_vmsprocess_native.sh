#!/bin/sh
# run_vmsprocess_native.sh — the FIRST real OVMX-library migration onto the
# VMS-native toolchain (bead vms-b65.1, pillar vms-ade). Proves that the actual
# src/vmsprocess library links VMS-native into LIBVMSPROCESS$SHR.EXE, activates
# through IMGACT.EXE, and a consumer that imports its universals gets the
# VMS-correct result — with NO ld / NO ld.so. This is the TEMPLATE for the whole
# b65 chain (libvms -> vmslnm -> vmsfs -> vmsrms -> DCL).
#
# It combines the two spine increments already merged:
#   - vms-e65 (run_shareable_import_activation.sh): a lib shareable binds its own
#     libc/pthread CALL imports to DECC$SHR; IMGACT resolves them transitively.
#   - vms-616 (run_tls_producer_over_crtl.sh): a TLS-bearing producer coexists with
#     the C-RTL's thread-pointer ownership.
# vmsprocess needs BOTH at once: it imports pthread/malloc/... from DECC$SHR AND is
# a TLS producer. Its per-thread state is consolidated into a SINGLE TLS-defining
# object (vms_pcb.c's `current_pcb`); vms_get_current_process caches its snapshot in
# the PCB (pcb->cached_process) rather than a second __thread object, so the image
# has exactly ONE TLS object — what the VMS-native linker supports today (vms-b65.1;
# general multi-object TLS is tracked in vms-212).
#
# Chain, all VMS-native (no ld / no ld.so):
#   1. build IMGACT.EXE + LINK.EXE.
#   2. mk_decc_shr.sh whole-archives musl libc.a + libgcc.a into DECC$SHR.EXE
#      (the C-RTL; owns TP at activation). Installed in SYS$SHARE.
#   3. mk_vmsprocess_shr.sh compiles the 5 real vmsprocess objects and links
#      LIBVMSPROCESS$SHR.EXE via LINK.EXE --shareable --use DECC$SHR: it EXPORTS
#      the process-control universals (vms_pcb_*, eflag_*, ast_*, ...) and IMPORTS
#      pthread_mutex/cond_*, calloc/malloc/free, memset/strncpy/snprintf/sscanf,
#      getpid, close/raise/sigaction/sigemptyset FROM DECC$SHR. STRICT link: every
#      import MUST bind (no --allow-undefined).
#   4. LINK.EXE --executable --use LIBVMSPROCESS$SHR builds a consumer that imports
#      ONLY vmsprocess universals (vms_pcb_init + eflag_set) — it never names
#      DECC$SHR, so the DECC$SHR bind is purely TRANSITIVE.
#   5. RUN the consumer FOR REAL: kernel -> IMGACT.EXE -> load LIBVMSPROCESS$SHR ->
#      (transitively) load DECC$SHR, bind vmsprocess's pthread/calloc imports ->
#      bind the consumer's vms_pcb_init/eflag_set imports -> drive musl __init_libc
#      (DECC$SHR owns TP) -> ABSORB vmsprocess's TLS module against musl's TP ->
#      transfer control. The consumer:
#        vms_pcb_init(0)  -> allocates the PCB, pthread_mutex/cond_init x5 (DECC$SHR),
#                            writes current_pcb (TLS).
#        eflag_set(5)     -> vms_pcb_get() reads current_pcb (TLS, coexisting with
#                            musl TP), locks pcb->ef_lock (DECC$SHR pthread), flag 5
#                            was CLEAR  -> returns SS$_WASCLR = 1.
#        eflag_set(5)     -> flag 5 now SET -> returns SS$_WASSET = 9.
#      Consumer exits 42 iff (pcb != NULL && rc1 == 1 && rc2 == 9). Exit 42 proves:
#        - the vmsprocess universals bound in the consumer,
#        - the __thread PCB pointer round-tripped through TLSDESC against musl's TP
#          across TWO functions (write in vms_pcb_init, read in eflag_set) — the
#          single-TLS-object producer was absorbed correctly against musl's TP,
#        - the pthread_mutex/cond path bound transitively to DECC$SHR and ran,
#        - VMS event-flag semantics (WASCLR then WASSET) are exactly correct.
#
# Uses the arm64 musl container's libc.a + libgcc.a (aarch64-only for now,
# CLAUDE.md test loop). Needs root to create /vms. Exit 0 on ok.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
VMSPROC_DIR=$(cd "$IMGACT_DIR/../vmsprocess" && pwd)
LIBVMS_INC=$(cd "$IMGACT_DIR/../libvms/include" && pwd)
WORK=${WORK:-/tmp/vmsprocess-native}
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

echo "== mk_vmsprocess_shr.sh: real src/vmsprocess -> LIBVMSPROCESS\$SHR.EXE =="
# STRICT link inside the recipe (no --allow-undefined). If this fails on an
# unresolved libc symbol, DECC$SHR's vector needs that universal appended.
CC="$CC" sh "$LINK_DIR/mk_vmsprocess_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "" "$VMSPROC_DIR" "$LIBVMS_INC"

echo "-- LIBVMSPROCESS\$SHR.EXE: PT_TLS + symbol vector + .vms\$tls + its own imports --"
readelf -lW "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" | grep -E '\bTLS\b' || true
readelf -SW "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" | grep -E '\.tdata|\.tbss|\.vms\$tls|\.vms\$sv|\.vms\$imp|\.plt|\.igot' || true
readelf -lW "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" | grep -q '\bTLS\b'    || { echo "FAIL: producer has no PT_TLS (expected: current_pcb __thread)"; exit 1; }
readelf -SW "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" | grep -q '\.tbss'     || { echo "FAIL: producer emitted no .tbss"; exit 1; }
readelf -SW "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" | grep -q '\.vms\$tls' || { echo "FAIL: producer emitted no .vms\$tls"; exit 1; }
readelf -SW "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" | grep -q '\.vms\$sv'  || { echo "FAIL: producer emitted no .vms\$sv (no universals)"; exit 1; }
readelf -SW "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" | grep -q '\.vms\$imp' || { echo "FAIL: producer emitted no .vms\$imp (imports not bound to DECC\$SHR)"; exit 1; }

echo "== LINK.EXE --executable --use LIBVMSPROCESS\$SHR -> consumer =="
# Imports ONLY vmsprocess universals (vms_pcb_init + eflag_set). The DECC$SHR bind
# is purely transitive (IMGACT must pull it from LIBVMSPROCESS$SHR's .vms$imp).
cat > "$WORK/cons.c" <<'EOF'
extern void *vms_pcb_init(unsigned long initial_privs);
extern int   eflag_set(unsigned int efn);

void _start(void)
{
    void *pcb = vms_pcb_init(0);   /* PCB alloc + pthread_*_init (DECC$SHR); TLS set */
    int rc1 = eflag_set(5);        /* flag 5 was CLEAR -> SS$_WASCLR = 1 */
    int rc2 = eflag_set(5);        /* flag 5 now  SET  -> SS$_WASSET = 9 */

    int code = (pcb != 0 && rc1 == 1 && rc2 == 9) ? 42 : 1;

    register long x8 __asm__("x8") = 94;   /* exit_group */
    register long x0 __asm__("x0") = code;
    __asm__ volatile("svc 0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
EOF
$CC -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics \
    -c -o "$WORK/cons.o" "$WORK/cons.c"
"$WORK/LINK.EXE" --executable --use "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    -o "$WORK/CONS.EXE" "$WORK/cons.o"
chmod +x "$WORK/CONS.EXE"

echo
echo "== RUN ./CONS.EXE FOR REAL (kernel -> IMGACT -> LIBVMSPROCESS\$SHR -> DECC\$SHR) =="
set +e
"$WORK/CONS.EXE"; RC=$?
set -e
echo "exit code = $RC (expect 42 = pcb!=NULL && eflag_set#1==SS\$_WASCLR(1) && #2==SS\$_WASSET(9))"
[ "$RC" -eq 42 ] || { echo "FAIL: vmsprocess VMS-native migration did not run correctly (got $RC, want 42)"; exit 1; }

echo
echo "MILESTONE (vms-b65.1): the REAL src/vmsprocess library links VMS-native into"
echo "LIBVMSPROCESS\$SHR.EXE (its libc/pthread imports bound to DECC\$SHR, its"
echo "single-object __thread TLS absorbed against musl's TP), activates through"
echo "IMGACT.EXE, and a consumer gets VMS-correct event-flag results. The template"
echo "for the whole b65 lib-migration chain."
