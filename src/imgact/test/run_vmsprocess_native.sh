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
#   3. mk_vmsprocess_shr.sh compiles the 4 real vmsprocess objects and links
#      LIBVMSPROCESS$SHR.EXE via LINK.EXE --shareable --use DECC$SHR: it EXPORTS
#      the process-control universals (vms_pcb_*, ast_*, access_mode_*, priv_*,
#      ...) and IMPORTS pthread_mutex/cond_*, calloc/malloc/free,
#      memset/strncpy/snprintf/sscanf, getpid, close/raise/sigaction/sigemptyset
#      FROM DECC$SHR. STRICT link: every import MUST bind (no --allow-undefined).
#   4. LINK.EXE --executable --use LIBVMSPROCESS$SHR builds a consumer that imports
#      ONLY vmsprocess universals (vms_pcb_init + vms_pcb_get + access_mode_set) —
#      it never names DECC$SHR, so the DECC$SHR bind is purely TRANSITIVE.
#   5. RUN the consumer FOR REAL: kernel -> IMGACT.EXE -> load LIBVMSPROCESS$SHR ->
#      (transitively) load DECC$SHR, bind vmsprocess's pthread/calloc imports ->
#      bind the consumer's imports -> drive musl __init_libc (DECC$SHR owns TP) ->
#      ABSORB vmsprocess's TLS module against musl's TP -> transfer control.
#      The consumer:
#        vms_pcb_init(0)     -> allocates the PCB, pthread_mutex_init x3 (DECC$SHR),
#                               writes current_pcb (__thread, in vms_pcb.c).
#        vms_pcb_get()       -> reads current_pcb back; must be the SAME pointer.
#        access_mode_set(3)  -> defined in a DIFFERENT object (access_modes.c); it
#                               calls vms_pcb_get(). With the TLS PCB reachable it
#                               returns SS$_NORMAL = 1; if the TLS did not reach
#                               the second object it takes `if (!pcb)` and returns
#                               SS$_BADPARAM = 20. That is the discriminator.
#      Consumer exits 42 iff (pcb != NULL && got == pcb && rc == 1). Exit 42 proves:
#        - the vmsprocess universals bound in the consumer,
#        - the __thread PCB pointer round-tripped through TLSDESC against musl's TP
#          across TWO OBJECTS (written in vms_pcb.c, read in access_modes.c) — the
#          single-TLS-object producer was absorbed correctly against musl's TP,
#        - the pthread_mutex/cond path bound transitively to DECC$SHR and ran.
#
# SUBJECT NOTE (vms-2a8). This test used to import eflag_set and assert
# SS$_WASCLR-then-SS$_WASSET on event flag 5. That certified per-process event
# flags end-to-end — the exact facade CLAUDE.md Rule 11 forbids, since event
# flags are executive-resident (src/kernel/vms_eflag.c via /dev/vms) and a
# per-process implementation is the ONLY thing that could have passed it. The
# implementation was deleted, so the probe moved to a subject with no shared-state
# claim at all: the current access mode is per-thread PSL state in VMS, not a
# system facility, and it is used here purely as a cross-object TLS probe. This
# suite's subject is the VMS-NATIVE TOOLCHAIN (LINK.EXE/IMGACT/TLS/import binding);
# it is not, and must not become, a certification of any VMS system facility.
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

# ---------------------------------------------------------------------------
# The eight event-flag universals are RETIRED IN PLACE (vms-2a8). event_flags.c
# was the per-process event-flag facade and is gone; under the public VMS
# upward-compatibility rules (§5.1/§5.3, docs/design-link-native-toolchain.md) a
# universal that goes away is retired with PRIVATE_PROCEDURE, never deleted,
# so that every LATER entry keeps the index a consumer already bound.
# Both halves are asserted here, because either one alone is worthless:
#   (a) the slots are still there, at their original indices 9..16, marked
#       RETIRED — so ast_*/access_mode_*/priv_* did not shift; and
#   (b) a consumer that names a retired universal CANNOT link against it.
# ---------------------------------------------------------------------------
echo "-- retired event-flag slots: positions kept, binding refused --"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/OVMXDUMP" "$LINK_DIR/dump_image.c"
"$WORK/OVMXDUMP" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" > "$WORK/vmsprocess.sv"
sed -n '/symbol vector/,$p' "$WORK/vmsprocess.sv" | sed -n '1,40p'

NRET=$(grep -c 'RETIRED' "$WORK/vmsprocess.sv" || true)
[ "$NRET" -eq 8 ] || { echo "FAIL: expected 8 RETIRED symbol-vector slots, found $NRET"; exit 1; }
for i in 9 10 11 12 13 14 15 16; do
    grep -qE "^    \[ *$i\] RETIRED " "$WORK/vmsprocess.sv" || {
        echo "FAIL: symbol-vector index $i is not RETIRED (the eflag_* slots must stay in place)"; exit 1; }
done
# Placement proof: the first entry AFTER the retired run is still ast_init at 17.
grep -qE "^    \[ *17\] PROCEDURE .* ast_init$" "$WORK/vmsprocess.sv" || {
    echo "FAIL: ast_init is no longer at symbol-vector index 17 — the vector shifted"; exit 1; }

cat > "$WORK/retired.c" <<'EOF'
extern int eflag_set(unsigned int efn);
void _start(void) { eflag_set(5); }
EOF
$CC -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics \
    -c -o "$WORK/retired.o" "$WORK/retired.c"
set +e
"$WORK/LINK.EXE" --executable --use "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    -o "$WORK/RETIRED.EXE" "$WORK/retired.o" > "$WORK/retired.log" 2>&1
RRC=$?
set -e
cat "$WORK/retired.log"
[ "$RRC" -ne 0 ] || { echo "FAIL: LINK.EXE bound a RETIRED universal (eflag_set) — retirement is not enforced"; exit 1; }
echo "ok: LINK.EXE refused to bind the retired universal eflag_set (rc=$RRC)"

echo "== LINK.EXE --executable --use LIBVMSPROCESS\$SHR -> consumer =="
# Imports ONLY vmsprocess universals (vms_pcb_init + vms_pcb_get + access_mode_set).
# The DECC$SHR bind is purely transitive (IMGACT must pull it from
# LIBVMSPROCESS$SHR's .vms$imp).
cat > "$WORK/cons.c" <<'EOF'
extern void         *vms_pcb_init(unsigned long initial_privs);
extern void         *vms_pcb_get(void);
extern unsigned int  access_mode_set(unsigned char mode);   /* access_modes.c */

#define PSL$C_USER      3
#define SS$_NORMAL      1
#define SS$_BADPARAM    20   /* what access_mode_set returns when !vms_pcb_get() */

void _start(void)
{
    void *pcb = vms_pcb_init(0);   /* PCB alloc + pthread_*_init (DECC$SHR); TLS set */
    void *got = vms_pcb_get();     /* same object (vms_pcb.c): TLS read-back        */
    /* access_modes.c is a DIFFERENT object in the same image: it calls
     * vms_pcb_get() itself. SS$_NORMAL here means the __thread PCB pointer was
     * visible across objects; SS$_BADPARAM would mean it was not. */
    unsigned int rc = access_mode_set(PSL$C_USER);

    int code = (pcb != 0 && got == pcb && rc == SS$_NORMAL) ? 42 : 1;

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
echo "exit code = $RC (expect 42 = pcb!=NULL && vms_pcb_get()==pcb && access_mode_set(USER)==SS\$_NORMAL(1))"
[ "$RC" -eq 42 ] || { echo "FAIL: vmsprocess VMS-native migration did not run correctly (got $RC, want 42)"; exit 1; }

echo
echo "MILESTONE (vms-b65.1): the REAL src/vmsprocess library links VMS-native into"
echo "LIBVMSPROCESS\$SHR.EXE (its libc/pthread imports bound to DECC\$SHR, its"
echo "single-object __thread TLS absorbed against musl's TP), activates through"
echo "IMGACT.EXE, and a consumer's imports bind and run for real. The template"
echo "for the whole b65 lib-migration chain."
