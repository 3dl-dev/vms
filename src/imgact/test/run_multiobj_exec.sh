#!/bin/sh
# run_multiobj_exec.sh — the executable-emit milestone (bead vms-ba1, pillar
# vms-ade). Proves LINK.EXE's rebuilt emit_executable links a REAL multi-object,
# main()-entered C program — NOT a hand-rolled freestanding _start fixture — that:
#   * spans TWO translation units (mprog.c defines main + calls into mhelp.c),
#   * reads argc AND argv[0] off the initial process stack,
#   * references a .rodata string table (forcing ADR_PREL_PG_HI21 / ADD_ABS_LO12
#     intra-image relocations) AND a const pointer table (ABS64 -> .vms$rel),
#   * calls libc (printf + strlen) through DECC$SHR,
# links VMS-native via `LINK.EXE --executable --use DECC$SHR`, activates through
# IMGACT.EXE, and RUNS CORRECTLY — printing via the C-RTL and exiting with a code
# COMPUTED from argc — with NO ld / NO ld.so.
#
# What each piece exercises in the rebuilt emit_executable (vms-ba1):
#   1. MULTI-OBJECT linking      — mprog.o + mhelp.o merged on the SAME section-
#      by-ELF-flag-bucket machinery emit_shareable uses (not the old single-object
#      load_obj(ins[0]) toy). main->word_len / main->banner are intra-image CALL26.
#   2. FULL intra-image relocs   — the .rodata format strings ("%s argc=%d ...")
#      resolve via ADR_PREL_PG_HI21 + ADD_ABS_LO12_NC; g_words[] (const char*const
#      table) carries ABS64 pointer initializers biased through .vms$rel at
#      activation. (The old emit_executable rejected any non-CALL/GOT .text reloc.)
#   3. crt0                      — LINK synthesizes the entry stub (e_entry): it
#      recovers argc/argv/envp off SP (kernel-built, preserved across IMGACT's
#      _start bl/ret) and calls main(argc,argv,envp), then tail-calls exit().
#   4. IMGACT process-startup    — musl __init_libc was already driven by IMGACT
#      from DECC$SHR (vms-61f.2) before control transfer, so printf/strlen are
#      live and exit() flushes stdout. The kernel-provided argc/argv/envp/auxv
#      stack IS the process-startup vector; crt0 consumes it directly.
#
# Exit code is argc + strlen("beta")(=4): 1+4=5 with no args, 3+4=7 with two args
# — proving argc is really read from the stack, not a constant. Runs under arm64
# emulation (CLAUDE.md test loop; needs the musl container's libc.a + libgcc.a and
# root to create /vms). Exit 0 on success.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
WORK=${WORK:-/tmp/multiobj-exec}
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

echo "== LINK.EXE: whole-archive musl -> DECC\$SHR.EXE (into SYS\$SHARE) =="
# Minimal C-RTL vector: the universals the program + crt0 bind (printf, strlen,
# exit) plus musl's __init_libc so IMGACT drives the runtime bootstrap by name.
VEC="printf=PROCEDURE,strlen=PROCEDURE,exit=PROCEDURE,__init_libc=PROCEDURE"
"$WORK/LINK.EXE" --shareable --symbol-vector "$VEC" \
    --gsmatch LEQUAL,1,0 -o "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
readelf -SW "$SYSLIB/DECC\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: DECC\$SHR no symbol vector"; exit 1; }

echo "== compile a TWO-object main() C program (gcc -fPIC, real stdio) =="
cat > "$WORK/mhelp.c" <<'EOF'
#include <string.h>
/* A const pointer TABLE -> .data.rel.ro: 4 ABS64 pointer initializers the linker
 * must resolve intra-image and record in .vms$rel (biased at activation). */
const char *const g_words[] = { "alpha", "beta", "gamma", "delta" };
int word_len(int i) { return (int)strlen(g_words[i & 3]); }  /* strlen -> DECC$SHR */
const char *banner(void) { return "OVMX-MULTIOBJ"; }
EOF
cat > "$WORK/mprog.c" <<'EOF'
#include <stdio.h>
extern const char *banner(void);      /* defined in mhelp.c (intra-image CALL26) */
extern int word_len(int i);           /* defined in mhelp.c (intra-image CALL26) */
int main(int argc, char **argv)
{
    /* .rodata format strings -> ADR_PREL_PG_HI21 + ADD_ABS_LO12_NC relocs.
     * argv[0] is read off the kernel-built initial stack (proves crt0 argv). */
    printf("%s argc=%d arg0=%s\n", banner(), argc, argv[0]);
    int code = argc + word_len(1);    /* argc + strlen("beta")(=4) */
    printf("computed=%d\n", code);
    return code;                      /* crt0 tail-calls exit(code) */
}
EOF
CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics"
$CC $CFLAGS -c -o "$WORK/mhelp.o" "$WORK/mhelp.c"
$CC $CFLAGS -c -o "$WORK/mprog.o" "$WORK/mprog.c"

echo "== LINK.EXE --executable --use DECC\$SHR -> MULTIPROG.EXE (multi-object) =="
"$WORK/LINK.EXE" --executable --use "$SYSLIB/DECC\$SHR.EXE" \
    -o "$WORK/MULTIPROG.EXE" "$WORK/mprog.o" "$WORK/mhelp.o"
chmod +x "$WORK/MULTIPROG.EXE"

echo "-- MULTIPROG.EXE: PT_INTERP=IMGACT, PT_PHDR, .vms\$imp (printf/strlen/exit) --"
readelf -lW "$WORK/MULTIPROG.EXE" | grep -E '\bINTERP\b|\bPHDR\b' || true
readelf -SW "$WORK/MULTIPROG.EXE" | grep -E '\.vms\$imp|\.vms\$rel|\.plt|\.igot' || true
readelf -lW "$WORK/MULTIPROG.EXE" | grep -q 'INTERP' || { echo "FAIL: no PT_INTERP"; exit 1; }
readelf -lW "$WORK/MULTIPROG.EXE" | grep -q 'PHDR'   || { echo "FAIL: no PT_PHDR"; exit 1; }
readelf -SW "$WORK/MULTIPROG.EXE" | grep -q '\.vms\$imp' || { echo "FAIL: no .vms\$imp"; exit 1; }
readelf -SW "$WORK/MULTIPROG.EXE" | grep -q '\.vms\$rel' || { echo "FAIL: no .vms\$rel (ABS64 g_words[] not recorded)"; exit 1; }

echo
echo "== RUN ./MULTIPROG.EXE FOR REAL, no args (kernel -> IMGACT -> DECC\$SHR) =="
set +e
OUT=$("$WORK/MULTIPROG.EXE"); RC=$?
set -e
echo "$OUT"
echo "exit code = $RC (expect 5 = argc(1) + strlen(\"beta\")(4))"
echo "$OUT" | grep -q 'OVMX-MULTIOBJ argc=1 arg0=' || { echo "FAIL: banner/argc/argv line wrong"; exit 1; }
echo "$OUT" | grep -q 'computed=5' || { echo "FAIL: computed line wrong (printf/intra-image call)"; exit 1; }
[ "$RC" -eq 5 ] || { echo "FAIL: multi-object exec did not run correctly (got $RC, want 5)"; exit 1; }

echo
echo "== RUN ./MULTIPROG.EXE FOR REAL, TWO args (proves argc read from stack) =="
set +e
OUT2=$("$WORK/MULTIPROG.EXE" alpha beta); RC2=$?
set -e
echo "$OUT2"
echo "exit code = $RC2 (expect 7 = argc(3) + strlen(\"beta\")(4))"
echo "$OUT2" | grep -q 'argc=3' || { echo "FAIL: argc not 3 with two args (argv/argc off stack wrong)"; exit 1; }
[ "$RC2" -eq 7 ] || { echo "FAIL: argc-dependent exit wrong (got $RC2, want 7)"; exit 1; }

echo
echo "MILESTONE (vms-ba1): a REAL multi-object main() C program (mprog.o + mhelp.o,"
echo ".rodata string table + ABS64 pointer table + libc calls) links VMS-native via"
echo "LINK.EXE --executable --use DECC\$SHR, activates through IMGACT.EXE, reads argc"
echo "off the process stack via a synthesized crt0, calls printf/strlen through the"
echo "C-RTL, and exits with an argc-computed code — NO ld / NO ld.so. Unblocks"
echo "vms-c86 (executable TLS) and vms-b65.6 (DCL-native)."
