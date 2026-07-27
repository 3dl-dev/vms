#!/bin/sh
# run_shareable_import_activation.sh — the lib-shareable transitive-import
# milestone (pillar vms-ade, bead vms-e65). Proves that a LIBRARY shareable
# (not just a leaf executable) can bind its own libc/pthread CALL imports to
# DECC$SHR, VMS-native, and that IMGACT.EXE resolves that binding TRANSITIVELY
# at activation. This is the spine increment that unblocks the whole b65
# lib-migration chain (libvms/vmslnm/vmsfs/vmsrms/DCL onto LINK.EXE + DECC$SHR).
#
# Chain, all VMS-native (no ld / no ld.so):
#   1. build IMGACT.EXE (+ transitive .vms$imp resolution) + LINK.EXE.
#   2. mk_decc_shr.sh whole-archives musl libc.a + libgcc.a into DECC$SHR.EXE,
#      whose .vms$sv now also exports pthread_mutex_lock/unlock (+ the rest of
#      the b65 pthread/signal set). Installed in SYS$SHARE.
#   3. LINK.EXE --shareable --use DECC$SHR builds TESTLIB$SHR.EXE, a LIB shareable
#      that EXPORTS lib_compute (a universal) and IMPORTS pthread_mutex_lock/
#      unlock + malloc + memset + free FROM DECC$SHR. Before vms-e65 this was
#      impossible: emit_shareable left those CALL26 sites unpatched (branch-to-
#      garbage), emitted no PLT and no .vms$imp. Now it emits a PLT + import GOT +
#      .vms$imp bound to DECC$SHR.
#   4. LINK.EXE --executable --use TESTLIB$SHR builds a consumer that imports ONLY
#      lib_compute — it never names DECC$SHR. That makes the DECC$SHR bind purely
#      TRANSITIVE (IMGACT must pull it from TESTLIB$SHR's own .vms$imp).
#   5. RUN the consumer FOR REAL: kernel -> IMGACT.EXE -> load TESTLIB$SHR ->
#      (transitively) load DECC$SHR, apply its .vms$rel, bind TESTLIB$SHR's
#      pthread/malloc imports into TESTLIB$SHR's import GOT -> bind the consumer's
#      lib_compute import -> drive musl __init_libc (DECC$SHR is the C-RTL) ->
#      transfer control. The consumer calls lib_compute(41), which locks a mutex,
#      malloc+memset+writes a buffer, frees it, unlocks — all through DECC$SHR —
#      and returns 42. Exit 42 proves the lib shareable's OWN imports transitively
#      bound to DECC$SHR and a real pthread/malloc path ran through it.
#
# Uses the arm64 musl container's libc.a + libgcc.a (aarch64-only for now,
# CLAUDE.md test loop). Needs root to create /vms. Exit 0 on ok.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
WORK=${WORK:-/tmp/shr-import-act}
rm -rf "$WORK"; mkdir -p "$WORK"

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need arm64 musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }

echo "== build IMGACT.EXE (transitive .vms\$imp resolution) + LINK.EXE =="
( cd "$IMGACT_DIR" && make CC="$CC" clean >/dev/null 2>&1 || true; make CC="$CC" ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

echo "== mk_decc_shr.sh: whole-archive musl -> DECC\$SHR.EXE (production vector) =="
# Use the PRODUCTION recipe so the run exercises the appended pthread/signal
# universals (vms-e65 part 3), not a hand-rolled test vector.
sh "$LINK_DIR/mk_decc_shr.sh" "$WORK/LINK.EXE" "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
readelf -SW "$SYSLIB/DECC\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: DECC\$SHR has no symbol vector"; exit 1; }

echo "== LINK.EXE --shareable --use DECC\$SHR -> TESTLIB\$SHR.EXE (lib shareable) =="
# A library shareable: EXPORTS lib_compute; IMPORTS pthread_mutex_lock/unlock +
# malloc + memset + free from DECC$SHR. Freestanding + -fno-builtin so memset
# stays a real CALL26 import (not inlined); -mno-outline-atomics so gcc does not
# emit __aarch64_* outline-atomic helper calls the C-RTL does not export.
cat > "$WORK/testlib.c" <<'EOF'
extern void *malloc(unsigned long);
extern void  free(void *);
extern void *memset(void *, int, unsigned long);
extern int   pthread_mutex_lock(void *);
extern int   pthread_mutex_unlock(void *);

/* musl PTHREAD_MUTEX_INITIALIZER is all-zero; a zeroed object is a default
 * (normal) mutex. Generous size + 8-byte alignment for musl's layout. */
static long g_mtx[8];

/* The exported universal. Exercises a real pthread + malloc path that lives in
 * DECC$SHR — reached through THIS shareable's own import GOT (bound transitively
 * by IMGACT). Returns x + 1. */
int lib_compute(int x)
{
    pthread_mutex_lock(g_mtx);
    unsigned char *buf = malloc(16);
    memset(buf, 0, 16);
    buf[0] = (unsigned char)(x + 1);
    int r = buf[0];
    free(buf);
    pthread_mutex_unlock(g_mtx);
    return r;
}
EOF
$CC -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics \
    -c -o "$WORK/testlib.o" "$WORK/testlib.c"
# STRICT (no --allow-undefined): every undefined symbol MUST bind to DECC$SHR.
"$WORK/LINK.EXE" --shareable --use "$SYSLIB/DECC\$SHR.EXE" \
    --symbol-vector "lib_compute=PROCEDURE" --gsmatch LEQUAL,1,0 \
    -o "$SYSLIB/TESTLIB\$SHR.EXE" "$WORK/testlib.o"
echo "-- TESTLIB\$SHR.EXE carries a symbol vector AND its own imports --"
readelf -SW "$SYSLIB/TESTLIB\$SHR.EXE" | grep -E '\.vms\$sv|\.vms\$imp|\.plt|\.igot' || true
readelf -SW "$SYSLIB/TESTLIB\$SHR.EXE" | grep -q '\.vms\$imp' || { echo "FAIL: lib shareable emitted no .vms\$imp (imports not bound)"; exit 1; }

echo "== LINK.EXE --executable --use TESTLIB\$SHR -> consumer (imports lib_compute only) =="
cat > "$WORK/cons.c" <<'EOF'
extern int lib_compute(int);
void _start(void)
{
    int rc = lib_compute(41);              /* -> 42, via DECC$SHR pthread/malloc */
    register long x8 __asm__("x8") = 94;   /* exit_group */
    register long x0 __asm__("x0") = rc;
    __asm__ volatile("svc 0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/cons.o" "$WORK/cons.c"
"$WORK/LINK.EXE" --executable --use "$SYSLIB/TESTLIB\$SHR.EXE" \
    -o "$WORK/CONS.EXE" "$WORK/cons.o"
chmod +x "$WORK/CONS.EXE"

echo
echo "== RUN ./CONS.EXE FOR REAL (kernel -> IMGACT -> TESTLIB\$SHR -> DECC\$SHR) =="
set +e
"$WORK/CONS.EXE"; RC=$?
set -e
echo "exit code = $RC (expect 42 = lib_compute(41): pthread+malloc+memset+free via DECC\$SHR)"
[ "$RC" -eq 42 ] || { echo "FAIL: transitive lib-shareable import bind did not run (got $RC, want 42)"; exit 1; }

echo
echo "MILESTONE: a LIBRARY shareable binds its OWN libc/pthread CALL imports to"
echo "DECC\$SHR (LINK.EXE emit_shareable), and IMGACT.EXE resolves that binding"
echo "TRANSITIVELY at activation — the increment that unblocks the b65 chain (vms-e65)."
