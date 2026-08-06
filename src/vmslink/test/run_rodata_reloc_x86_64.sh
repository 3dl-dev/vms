#!/bin/sh
# run_rodata_reloc_x86_64.sh — the vms-a66 regression gate: LINK.EXE must apply
# relocations against READ-ONLY allocatable sections (gcc's per-function switch
# jump tables, .eh_frame), not drop them.
#
# WHY THIS EXISTS, AND WHY THE EARLIER PROOFS MISSED IT
# ----------------------------------------------------
# LINK.EXE collected relocations only for sections it bucketed B_TEXT or B_DATA;
# every relocation whose target section was B_RODATA was discarded WITHOUT a
# diagnostic. gcc emits each `switch` jump table into `.rodata.<fn>` as
# `.long arm - table_base`, one R_X86_64_PC32 per arm (the arms are in .text and
# the table is in .rodata, so the assembler cannot fold the difference). Dropping
# them left every such table all zero, and the dispatch sequence
# `movslq (%rcx,%rsi,4),%rdx ; add %rcx,%rdx ; jmp *%rdx` therefore jumped to
# table_base + 0 and executed the table's own bytes.
#
# vms-206 (3-import multi-object exec), vms-cd1 (1 GOT data import) and vms-2e4
# (TLSDESC) all linked and ran clean through this bug because none of their tiny
# specimens contained a switch big enough for gcc to build a table, and none of
# them called a printf-family function with a conversion — so no jump table was
# ever executed. It was NOT a scale limit (import count, GOT size, TLS, --use
# chain depth); it was a code SHAPE that only appeared once real musl and the
# real DCL sources entered the link. This harness reproduces that shape directly,
# in a specimen small enough to run in seconds.
#
# WHAT IT ASSERTS
#   1. The compiled specimen really carries .rela.rodata* R_X86_64_PC32
#      relocations. If a future gcc stops emitting jump tables this FAILS —
#      a test that silently stops testing is worse than no test.
#   2. LINK.EXE emits no %LINK-W-RELSKIP naming a read-only target: it must not
#      drop them again (the diagnostic itself is part of the vms-a66 fix — the
#      original bug was silent).
#   3. GROUND SOURCE: the linked image, activated through a real IMGACT.EXE with
#      no ld / no ld.so, prints the exact expected transcript and exits 0. Both
#      the CONSUMER's own jump tables (classify/weigh, patched by LINK.EXE) and
#      the PRODUCER's (musl printf_core/pop_arg inside DECC$SHR) are executed.
#      Pre-fix this step dies with SIGSEGV/SI_KERNEL inside DECC$SHR's .rodata.
#
# Runs natively in an x86_64 musl container (alpine + gcc musl-dev binutils make
# linux-headers). Needs root to create /vms. Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/vmslink/test
LINK_DIR=$(cd "$HERE/.." && pwd)             # src/vmslink
IMGACT_DIR=$(cd "$LINK_DIR/../imgact" && pwd)
WORK=${WORK:-/tmp/rodata-reloc-x86_64}
rm -rf "$WORK"; mkdir -p "$WORK"

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "FAIL: no musl libc.a at $LIBC (need an x86_64 musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }

echo "== build IMGACT.EXE + LINK.EXE (x86_64) =="
( cd "$IMGACT_DIR" && make CC="$CC" ARCH=x86_64 clean >/dev/null 2>&1 || true
  make CC="$CC" ARCH=x86_64 ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

echo
echo "== DECC\$SHR.EXE (whole-archive musl — the REAL producer, same recipe as DCL) =="
sh "$LINK_DIR/mk_decc_shr.sh" "$WORK/LINK.EXE" "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC" \
    >"$WORK/decc.log" 2>&1 || { echo "FAIL: DECC\$SHR link failed"; tail -20 "$WORK/decc.log"; exit 1; }
tail -2 "$WORK/decc.log" | sed 's/^/   /'

echo
echo "== compile the specimen (same VMS-native flags DCL uses) =="
CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mtls-dialect=gnu2"
$CC $CFLAGS -c -o "$WORK/rodata_jumptable.o" "$HERE/rodata_jumptable.c"

echo
echo "== ASSERT 1: the object really carries jump tables in .rodata =="
readelf -SW "$WORK/rodata_jumptable.o" | grep -oE '\.rela\.rodata[^ ]*' | sed 's/^/   /' || true
NJT=$(readelf -rW "$WORK/rodata_jumptable.o" 2>/dev/null \
      | awk '/^Relocation section .\.rela\.rodata/{inj=1;next}
             /^Relocation section/{inj=0}
             inj && /R_X86_64_PC32/{n++} END{print n+0}')
echo "   R_X86_64_PC32 relocations against .rodata sections: $NJT"
[ "$NJT" -ge 20 ] || {
    echo "FAIL: the specimen produced only $NJT .rodata relocations (expected >= 20)."
    echo "  gcc no longer emits switch jump tables into .rodata for this source"
    echo "  (a switch whose arms all return CONSTANTS becomes a data lookup table,"
    echo "  which carries no relocations), so this harness would prove nothing."
    echo "  Fix the specimen — do NOT lower this bound."
    exit 1
}

echo
echo "== link JTPROG.EXE (LINK.EXE --executable --use DECC\$SHR) =="
"$WORK/LINK.EXE" --executable --use "$SYSLIB/DECC\$SHR.EXE" \
    -o "$SYSEXE/JTPROG.EXE" "$WORK/rodata_jumptable.o" 2>"$WORK/link.err"
sed 's/^/   /' "$WORK/link.err"

echo
echo "== ASSERT 2: LINK.EXE dropped no relocation against a read-only section =="
# %LINK-W-RELSKIP is the vms-a66 diagnostic: an allocatable target LINK.EXE does
# not place flat. A .rodata/.eh_frame target appearing here means the collector
# regressed to the silent-drop behaviour that produced the DCL.EXE segfault.
if grep -E 'RELSKIP.*(\.rodata|\.eh_frame)' "$WORK/link.err" "$WORK/decc.log"; then
    echo "FAIL: LINK.EXE skipped relocations against a read-only section (vms-a66 regression)"
    exit 1
fi
echo "   no %LINK-W-RELSKIP naming a read-only target"

echo
echo "== oracle: the SAME source built by the system toolchain (gcc + ld) =="
# The expected transcript is the native toolchain's answer to the same program,
# computed here — never a constant transcribed into this script. If LINK.EXE
# patches a jump table differently from ld, the diff shows it.
$CC -O2 -ffreestanding -fno-builtin -fno-stack-protector \
    -o "$WORK/REF" "$HERE/rodata_jumptable.c"
"$WORK/REF" > "$WORK/expect.txt"
set +e
"$WORK/REF" >/dev/null 2>&1
REFRC=$?
set -e
[ "$REFRC" -eq 0 ] || { echo "FAIL: the oracle build itself did not exit 0 ($REFRC)"; exit 1; }
echo "   oracle produced $(wc -l < "$WORK/expect.txt") lines, exit 0"

echo
echo "== ASSERT 3 (GROUND SOURCE): activate through IMGACT.EXE and diff vs oracle =="
readelf -lW "$SYSEXE/JTPROG.EXE" | grep -q 'INTERP' || { echo "FAIL: no PT_INTERP"; exit 1; }
set +e
"$SYSEXE/JTPROG.EXE" > "$WORK/out.txt" 2>&1
RC=$?
set -e
sed 's/^/   /' "$WORK/out.txt"
echo "   exit code = $RC"
grep -q '^PASS$' "$WORK/out.txt" || { echo "FAIL: JTPROG.EXE did not reach PASS"; exit 1; }
if ! diff -u "$WORK/expect.txt" "$WORK/out.txt" >"$WORK/diff.txt"; then
    echo "FAIL: transcript differs from the system-toolchain oracle — a jump table"
    echo "  was patched wrong (or not at all)"
    cat "$WORK/diff.txt"
    exit 1
fi
[ "$RC" -eq 0 ] || { echo "FAIL: JTPROG.EXE did not exit clean (got $RC)"; exit 1; }

echo
echo "ALL vms-a66 READ-ONLY-SECTION RELOCATION CHECKS PASSED"
echo "  (consumer jump tables patched by LINK.EXE + musl printf_core/pop_arg"
echo "   jump tables inside DECC\$SHR, both executed through a real IMGACT.EXE)"
