#!/bin/sh
# run_tls_producer_over_crtl.sh — a TLS-bearing lib shareable coexisting with the
# musl C-RTL (bead vms-616, pillar vms-ade). This is the LAST general spine
# prerequisite before the real OVMX-lib migrations (the b65 chain: vmsprocess,
# libvms, DCL — all of which use __thread).
#
# The problem it proves solved: vms-61f.2 made TLS ownership mutually exclusive —
# a C-RTL (DECC$SHR) owns the thread pointer via musl __init_libc, XOR IMGACT owns
# it for a standalone TLSDESC producer. But a real lib shareable is a TLS PRODUCER
# (its own .tdata/.tbss + TLSDESC) that ALSO imports from DECC$SHR — it must
# COEXIST with musl's TP ownership. vms-616 teaches IMGACT to absorb such a
# producer's TLS module into the running process: it gives the module its own
# per-thread block and biases the module's static TLSDESC entries relative to
# musl's already-programmed TP.
#
# Chain, all VMS-native (no ld / no ld.so):
#   1. build IMGACT.EXE + LINK.EXE.
#   2. mk_decc_shr.sh whole-archives musl libc.a + libgcc.a into DECC$SHR.EXE
#      (the C-RTL; owns TP at activation). Installed in SYS$SHARE.
#   3. LINK.EXE --shareable --use DECC$SHR builds TLSLIB$SHR.EXE: a lib shareable
#      that (a) has its OWN thread-locals — one in .tdata (nonzero init image),
#      one in .tbss (zero) — accessed via TLSDESC, AND (b) IMPORTS malloc/memset/
#      free from DECC$SHR. So it is simultaneously a TLS producer and a DECC$SHR
#      consumer — the exact combination the old guard rejected.
#   4. LINK.EXE --executable --use TLSLIB$SHR builds a consumer that imports ONLY
#      lib_compute (never names DECC$SHR — the DECC$SHR bind is purely transitive).
#   5. RUN the consumer FOR REAL: kernel -> IMGACT.EXE -> load TLSLIB$SHR ->
#      (transitively) load DECC$SHR -> bind TLSLIB$SHR's malloc/memset/free
#      imports -> drive musl __init_libc (DECC$SHR owns TP) -> ABSORB TLSLIB$SHR's
#      TLS module against musl's TP -> transfer control. lib_compute(35) reads its
#      .tbss local (0) += 35, adds its .tdata local (7), and round-trips the sum
#      through a DECC$SHR malloc/memset/free path -> returns 42. Exit 42 proves:
#        - the .tdata init image was copied (t_init == 7),
#        - the .tbss local was zeroed (t_accum started 0),
#        - TLSDESC resolution against MUSL's TP works (both locals read correctly),
#        - the producer's malloc/memset/free imports bound to DECC$SHR still work,
#      i.e. a TLS producer and the C-RTL coexist in one process.
#
# Uses the arm64 musl container's libc.a + libgcc.a (aarch64-only for now,
# CLAUDE.md test loop). Needs root to create /vms. Exit 0 on ok.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
WORK=${WORK:-/tmp/tls-over-crtl}
rm -rf "$WORK"; mkdir -p "$WORK"

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need arm64 musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }

echo "== build IMGACT.EXE (TLS-producer/C-RTL coexistence) + LINK.EXE =="
( cd "$IMGACT_DIR" && make CC="$CC" clean >/dev/null 2>&1 || true; make CC="$CC" ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

echo "== mk_decc_shr.sh: whole-archive musl -> DECC\$SHR.EXE (production vector) =="
sh "$LINK_DIR/mk_decc_shr.sh" "$WORK/LINK.EXE" "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
readelf -SW "$SYSLIB/DECC\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: DECC\$SHR has no symbol vector"; exit 1; }
# DECC$SHR must NOT itself carry a PT_TLS — musl's own TLS derives from the main
# program's PT_TLS (which OVMX consumers lack). If this ever changes the
# coexistence model must be revisited.
readelf -lW "$SYSLIB/DECC\$SHR.EXE" | grep -q '\bTLS\b' && { echo "FAIL: DECC\$SHR unexpectedly carries a PT_TLS"; exit 1; } || true

echo "== LINK.EXE --shareable --use DECC\$SHR -> TLSLIB\$SHR.EXE (TLS producer + importer) =="
# A library shareable that is BOTH a TLS producer and a DECC$SHR importer.
# -ffreestanding + -fno-builtin so memset stays a real CALL26 import (not inlined);
# -mno-outline-atomics so gcc emits no __aarch64_* helper calls DECC$SHR lacks.
cat > "$WORK/tlslib.c" <<'EOF'
extern void *malloc(unsigned long);
extern void  free(void *);
extern void *memset(void *, int, unsigned long);

/* NON-static (external linkage) so the compiler cannot prove t_init is never
 * modified elsewhere and constant-fold it away — both genuinely live in TLS and
 * are read through the TLSDESC sequence. t_init lands in .tdata (nonzero init
 * image that IMGACT must COPY), t_accum in .tbss (that IMGACT must ZERO). */
__thread int t_init = 7;   /* .tdata: nonzero init image (must be copied) */
__thread int t_accum;      /* .tbss:  zero-initialized (must be zeroed)   */

/* The exported universal. Exercises its OWN thread-locals (via TLSDESC, resolved
 * against musl's TP) AND a DECC$SHR malloc/memset/free path (transitive import).
 * lib_compute(35): t_accum(0)+=35 -> 35; + t_init(7) -> 42, round-tripped through
 * a malloc'd buffer. Returns 42. */
int lib_compute(int x)
{
    t_accum += x;
    unsigned char *buf = malloc(16);
    memset(buf, 0, 16);
    buf[0] = (unsigned char)(t_accum + t_init);
    int r = buf[0];
    free(buf);
    return r;
}
EOF
$CC -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics \
    -c -o "$WORK/tlslib.o" "$WORK/tlslib.c"
echo "-- TLSDESC relocations gcc emitted (proves __thread -> TLSDESC) --"
readelf -rW "$WORK/tlslib.o" | awk '/TLSDESC/{print $3}' | sort | uniq -c
readelf -rW "$WORK/tlslib.o" | grep -q 'TLSDESC' || { echo "FAIL: no TLSDESC relocs (compiler used a different TLS model)"; exit 1; }
# STRICT (no --allow-undefined): malloc/memset/free MUST bind to DECC$SHR.
"$WORK/LINK.EXE" --shareable --use "$SYSLIB/DECC\$SHR.EXE" \
    --symbol-vector "lib_compute=PROCEDURE" --gsmatch LEQUAL,1,0 \
    -o "$SYSLIB/TLSLIB\$SHR.EXE" "$WORK/tlslib.o"
echo "-- TLSLIB\$SHR.EXE carries PT_TLS + a symbol vector + .vms\$tls + its own imports --"
readelf -lW "$SYSLIB/TLSLIB\$SHR.EXE" | grep -E '\bTLS\b' || true
readelf -SW "$SYSLIB/TLSLIB\$SHR.EXE" | grep -E '\.tdata|\.tbss|\.vms\$tls|\.vms\$sv|\.vms\$imp|\.plt|\.igot' || true
readelf -lW "$SYSLIB/TLSLIB\$SHR.EXE" | grep -q '\bTLS\b'    || { echo "FAIL: producer has no PT_TLS"; exit 1; }
readelf -SW "$SYSLIB/TLSLIB\$SHR.EXE" | grep -q '\.tdata'    || { echo "FAIL: producer emitted no .tdata (nonzero TLS init image not present)"; exit 1; }
readelf -SW "$SYSLIB/TLSLIB\$SHR.EXE" | grep -q '\.tbss'     || { echo "FAIL: producer emitted no .tbss"; exit 1; }
readelf -SW "$SYSLIB/TLSLIB\$SHR.EXE" | grep -q '\.vms\$tls' || { echo "FAIL: producer emitted no .vms\$tls"; exit 1; }
readelf -SW "$SYSLIB/TLSLIB\$SHR.EXE" | grep -q '\.vms\$imp' || { echo "FAIL: producer emitted no .vms\$imp (imports not bound)"; exit 1; }

echo "== LINK.EXE --executable --use TLSLIB\$SHR -> consumer (imports lib_compute only) =="
cat > "$WORK/cons.c" <<'EOF'
extern int lib_compute(int);
void _start(void)
{
    int rc = lib_compute(35);              /* -> 42: TLS locals + DECC$SHR malloc */
    register long x8 __asm__("x8") = 94;   /* exit_group */
    register long x0 __asm__("x0") = rc;
    __asm__ volatile("svc 0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/cons.o" "$WORK/cons.c"
"$WORK/LINK.EXE" --executable --use "$SYSLIB/TLSLIB\$SHR.EXE" \
    -o "$WORK/CONS.EXE" "$WORK/cons.o"
chmod +x "$WORK/CONS.EXE"

echo
echo "== RUN ./CONS.EXE FOR REAL (kernel -> IMGACT -> TLSLIB\$SHR + DECC\$SHR) =="
set +e
"$WORK/CONS.EXE"; RC=$?
set -e
echo "exit code = $RC (expect 42 = t_accum(0)+35 + t_init(7), round-tripped via DECC\$SHR malloc)"
[ "$RC" -eq 42 ] || { echo "FAIL: TLS-producer/C-RTL coexistence did not run (got $RC, want 42)"; exit 1; }

echo
echo "MILESTONE: a TLS-bearing LIB shareable (its own .tdata/.tbss + TLSDESC) that"
echo "ALSO imports from DECC\$SHR activates through IMGACT — its TLS module absorbed"
echo "into musl's TP-owned view — with BOTH its __thread access AND its DECC\$SHR"
echo "imports working. The last spine prerequisite before the b65 lib migrations (vms-616)."
