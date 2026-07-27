#!/bin/sh
# run_exec_tls.sh — executable-own TLS (bead vms-c86, pillar vms-ade). The LAST
# emit_executable piece before DCL-native (vms-b65.6).
#
# vms-616 taught IMGACT to absorb a TLS-producing SHAREABLE's TLS module into
# musl's TP-owned view (give it its own block, bias its static TLSDESC entries
# relative to musl's TP). vms-ba1 made emit_executable link real multi-object
# main() programs. This test proves the combination: a MULTI-OBJECT EXECUTABLE
# that itself has a __thread/TLSDESC variable (like DCL's dcl_messages.o) gets
# its OWN thread-pointer offset in musl's combined TLS image at activation — so
# BOTH its own TLS access AND its DECC$SHR imports work, VMS-native (no ld/ld.so).
#
# The executable module (g_exe) is now a first-class TLS participant: it is NOT
# a producer (no .vms$sv, not in g_prods), so IMGACT records its PT_TLS + .vms$tls
# separately and absorbs it in the same C-RTL coexistence pass as producers.
#
# Chain, all VMS-native:
#   1. build IMGACT.EXE + LINK.EXE.
#   2. LINK.EXE --shareable whole-archives musl -> DECC$SHR.EXE (C-RTL; owns TP).
#   3. LINK.EXE --executable --use DECC$SHR builds TLSEXE.EXE from TWO objects:
#      tprog.o (main; has its OWN __thread t_msg in .tdata + t_accum in .tbss,
#      accessed via TLSDESC; calls printf) + thelp.o (word_len -> strlen).
#   4. RUN it FOR REAL: kernel -> IMGACT.EXE -> bind imports -> load DECC$SHR ->
#      drive musl __init_libc (DECC$SHR owns TP) -> ABSORB the EXECUTABLE's own
#      TLS module against musl's TP -> transfer to crt0 -> main.
#      main(argc): t_accum(0)+=argc; code = t_accum + t_msg(7) + strlen("beta")(4).
#      No args argc=1 -> 1+7+4 = 12. Two args argc=3 -> 3+7+4 = 14.
#      Exit 12/14 proves:
#        - the exe's .tdata init image was copied (t_msg == 7),
#        - the exe's .tbss local was zeroed (t_accum started 0),
#        - TLSDESC resolution of the EXECUTABLE's own locals against musl's TP works,
#        - the exe's printf/strlen imports still bound to DECC$SHR,
#        - argc is genuinely read off the process stack (12 vs 14).
#
# Uses the arm64 musl container's libc.a + libgcc.a (aarch64-only for now,
# CLAUDE.md test loop). Needs root to create /vms. Exit 0 on ok.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
WORK=${WORK:-/tmp/exec-tls}
rm -rf "$WORK"; mkdir -p "$WORK"

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need arm64 musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }

echo "== build IMGACT.EXE (executable-own TLS) + LINK.EXE =="
( cd "$IMGACT_DIR" && make CC="$CC" clean >/dev/null 2>&1 || true; make CC="$CC" ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

echo "== LINK.EXE: whole-archive musl -> DECC\$SHR.EXE (into SYS\$SHARE) =="
VEC="printf=PROCEDURE,strlen=PROCEDURE,exit=PROCEDURE,__init_libc=PROCEDURE"
"$WORK/LINK.EXE" --shareable --symbol-vector "$VEC" \
    --gsmatch LEQUAL,1,0 -o "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
readelf -SW "$SYSLIB/DECC\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: DECC\$SHR no symbol vector"; exit 1; }
# DECC$SHR must NOT itself carry a PT_TLS — musl's own TLS derives from the main
# program's PT_TLS, which is exactly the executable-own TLS this test exercises.
readelf -lW "$SYSLIB/DECC\$SHR.EXE" | grep -q '\bTLS\b' && { echo "FAIL: DECC\$SHR unexpectedly carries a PT_TLS"; exit 1; } || true

echo "== compile a TWO-object main() C program WHOSE MAIN IMAGE HAS ITS OWN __thread =="
cat > "$WORK/thelp.c" <<'EOF'
#include <string.h>
/* Second translation unit — NO TLS. Forces an intra-image CALL26 and a
 * DECC$SHR strlen import, proving multi-object linking still works. */
int word_len(const char *s) { return (int)strlen(s); }  /* strlen -> DECC$SHR */
EOF
cat > "$WORK/tprog.c" <<'EOF'
#include <stdio.h>
extern int word_len(const char *);   /* defined in thelp.c (intra-image CALL26) */

/* The EXECUTABLE's OWN thread-locals — the point of vms-c86. NON-static
 * (external linkage) so the compiler cannot constant-fold them; both genuinely
 * live in the main image's TLS and are read through the TLSDESC sequence.
 * t_msg lands in .tdata (nonzero init image the activator must COPY),
 * t_accum in .tbss (that the activator must ZERO). */
__thread int t_msg = 7;   /* .tdata: nonzero init image (must be copied) */
__thread int t_accum;     /* .tbss:  zero-initialized (must be zeroed)   */

int main(int argc, char **argv)
{
    t_accum += argc;                          /* .tbss local: 0 + argc */
    int code = t_accum + t_msg + word_len("beta");  /* + .tdata local(7) + 4 */
    /* printf via DECC$SHR; reads the exe's own TLS locals too. */
    printf("OVMX-EXEC-TLS argc=%d t_msg=%d t_accum=%d code=%d arg0=%s\n",
           argc, t_msg, t_accum, code, argv[0]);
    return code;                              /* crt0 tail-calls exit(code) */
}
EOF
# -fPIC: aarch64 default TLS dialect is `desc`, so external __thread -> TLSDESC.
# -ffreestanding -fno-builtin: keep strlen/printf as real DECC$SHR imports.
# -mno-outline-atomics: no __aarch64_* helper calls DECC$SHR lacks.
CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics"
$CC $CFLAGS -c -o "$WORK/thelp.o" "$WORK/thelp.c"
$CC $CFLAGS -c -o "$WORK/tprog.o" "$WORK/tprog.c"
echo "-- TLSDESC relocations gcc emitted in the MAIN object (proves __thread -> TLSDESC) --"
readelf -rW "$WORK/tprog.o" | awk '/TLSDESC/{print $3}' | sort | uniq -c
readelf -rW "$WORK/tprog.o" | grep -q 'TLSDESC' || { echo "FAIL: no TLSDESC relocs in main object (compiler used a different TLS model)"; exit 1; }

echo "== LINK.EXE --executable --use DECC\$SHR -> TLSEXE.EXE (exe with its OWN TLS) =="
"$WORK/LINK.EXE" --executable --use "$SYSLIB/DECC\$SHR.EXE" \
    -o "$WORK/TLSEXE.EXE" "$WORK/tprog.o" "$WORK/thelp.o"
chmod +x "$WORK/TLSEXE.EXE"

echo "-- TLSEXE.EXE: PT_INTERP=IMGACT, PT_PHDR, PT_TLS, .vms\$tls, .vms\$imp --"
readelf -lW "$WORK/TLSEXE.EXE" | grep -E '\bINTERP\b|\bPHDR\b|\bTLS\b' || true
readelf -SW "$WORK/TLSEXE.EXE" | grep -E '\.tdata|\.tbss|\.vms\$tls|\.vms\$imp|\.plt|\.igot' || true
readelf -lW "$WORK/TLSEXE.EXE" | grep -q 'INTERP'   || { echo "FAIL: no PT_INTERP"; exit 1; }
readelf -lW "$WORK/TLSEXE.EXE" | grep -q 'PHDR'     || { echo "FAIL: no PT_PHDR"; exit 1; }
readelf -lW "$WORK/TLSEXE.EXE" | grep -q '\bTLS\b'  || { echo "FAIL: executable has no PT_TLS (own __thread not emitted)"; exit 1; }
readelf -SW "$WORK/TLSEXE.EXE" | grep -q '\.tdata'  || { echo "FAIL: executable emitted no .tdata (nonzero TLS init image absent)"; exit 1; }
readelf -SW "$WORK/TLSEXE.EXE" | grep -q '\.vms\$tls' || { echo "FAIL: executable emitted no .vms\$tls (TLSDESC table absent)"; exit 1; }
readelf -SW "$WORK/TLSEXE.EXE" | grep -q '\.vms\$imp' || { echo "FAIL: executable emitted no .vms\$imp (imports not bound)"; exit 1; }

echo
echo "== RUN ./TLSEXE.EXE FOR REAL, no args (kernel -> IMGACT -> DECC\$SHR) =="
set +e
OUT=$("$WORK/TLSEXE.EXE"); RC=$?
set -e
echo "$OUT"
echo "exit code = $RC (expect 12 = t_accum(argc=1) + t_msg(7) + strlen(\"beta\")(4))"
echo "$OUT" | grep -q 'OVMX-EXEC-TLS argc=1 t_msg=7 t_accum=1 code=12 arg0=' \
    || { echo "FAIL: exec-TLS output wrong (own TLS locals or printf import broken)"; exit 1; }
[ "$RC" -eq 12 ] || { echo "FAIL: executable-own TLS did not activate correctly (got $RC, want 12)"; exit 1; }

echo
echo "== RUN ./TLSEXE.EXE FOR REAL, TWO args (proves argc read + .tbss re-zeroed per run) =="
set +e
OUT2=$("$WORK/TLSEXE.EXE" alpha beta); RC2=$?
set -e
echo "$OUT2"
echo "exit code = $RC2 (expect 14 = t_accum(argc=3) + t_msg(7) + 4)"
echo "$OUT2" | grep -q 'argc=3 t_msg=7 t_accum=3 code=14' \
    || { echo "FAIL: argc/TLS wrong with two args (t_accum should be 3, not accumulated)"; exit 1; }
[ "$RC2" -eq 14 ] || { echo "FAIL: argc-dependent exit wrong (got $RC2, want 14)"; exit 1; }

echo
echo "MILESTONE (vms-c86): a MULTI-OBJECT executable with its OWN __thread/TLSDESC"
echo "variable (like DCL's dcl_messages.o) links VMS-native via LINK.EXE --executable"
echo "--use DECC\$SHR, activates through IMGACT.EXE — its own TLS module absorbed into"
echo "musl's TP-owned view — with BOTH its __thread access AND its DECC\$SHR imports"
echo "working, NO ld / NO ld.so. The last emit_executable piece before DCL-native (b65.6)."
