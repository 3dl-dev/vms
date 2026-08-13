#!/bin/sh
# run_olb_roundtrip.sh — LIBRARIAN.EXE + .OLB + LINK.EXE round-trip proof (vms-ca9).
#
# Veracity (Q1/Q2): a REAL round-trip through the real files the tools read —
#   1. LIBRARIAN.EXE builds an .OLB (ar container) from >=2 real .OBJ members.
#   2. A stock `ar t` (independent oracle) confirms the container is a valid ar
#      archive holding exactly those members.
#   3. LINK.EXE resolves an undefined symbol by PULLING the one member that
#      defines it — selectively, leaving the unreferenced member out — and the
#      link SUCCEEDS with no --allow-undefined (so success PROVES the pull; had
#      the member not been pulled, LINK would abort "unresolved external").
#   4. Negative control: a library WITHOUT the needed member makes the same link
#      FAIL — proving the success in (3) is not a false positive.
#   5. The produced image is a valid OVMX ET_DYN shareable (OVMXDUMP parses its
#      symbol vector). Full IMGACT activation of a toolchain-produced image is
#      proven by the native container jobs (run_dcl_native.sh et al); this host
#      ctest proves the LIBRARIAN/.OLB/LINK mechanism and image validity.
#
# Inputs (env, set by CMake add_test): LINK_EXE, LIBRARIAN_EXE, OVMXDUMP, CC.
# Exit 0 = success.
set -e

: "${LINK_EXE:?need LINK_EXE}"
: "${LIBRARIAN_EXE:?need LIBRARIAN_EXE}"
: "${OVMXDUMP:?need OVMXDUMP}"
CC=${CC:-gcc}

command -v ar >/dev/null 2>&1 || { echo "SKIP-less FAIL: `ar` (binutils) required as the oracle"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

# Freestanding leaf objects: no libc references, so a link WITHOUT
# --allow-undefined has exactly one outstanding reference — `helper` — that only
# the library can satisfy.
CF="-O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -c"
case "$(uname -m)" in x86_64|amd64) CF="$CF -fcf-protection=none" ;; esac

printf 'int helper(void) { return 42; }\n'            > mod_a.c
printf 'int unused_fn(void) { return 7; }\n'          > mod_b.c
printf 'int helper(void);\nint entry(void) { return helper() + 1; }\n' > prog.c
$CC $CF mod_a.c -o MOD_A.o
$CC $CF mod_b.c -o MOD_B.o
$CC $CF prog.c  -o PROG.o

echo "== 1/5 LIBRARIAN /CREATE TESTLIB.OLB from 2 members =="
"$LIBRARIAN_EXE" /CREATE TESTLIB.OLB MOD_A.o MOD_B.o

echo "== 2/5 ar oracle: container holds MOD_A + MOD_B =="
MEMBERS=$(ar t TESTLIB.OLB | tr '\n' ' ')
echo "   ar t -> $MEMBERS"
echo "$MEMBERS" | grep -q "MOD_A" || { echo "FAIL: MOD_A not in .OLB per ar"; exit 1; }
echo "$MEMBERS" | grep -q "MOD_B" || { echo "FAIL: MOD_B not in .OLB per ar"; exit 1; }
"$LIBRARIAN_EXE" /LIST TESTLIB.OLB | grep -q "2 modules" || { echo "FAIL: LIST count"; exit 1; }

echo "== 3/5 LINK pulls ONLY the needed member (selective) =="
LOG=$("$LINK_EXE" --shareable --symbol-vector "entry=PROCEDURE" \
        --gsmatch LEQUAL,1,0 -o FOO.EXE PROG.o TESTLIB.OLB 2>&1)
echo "$LOG"
echo "$LOG" | grep -q "1 of 2 member" || { echo "FAIL: expected selective '1 of 2 members pulled'"; exit 1; }
echo "$LOG" | grep -q "%LINK-S-CREATED" || { echo "FAIL: link did not succeed"; exit 1; }
# 2 objects == PROG.o + MOD_A only (MOD_B was NOT pulled).
echo "$LOG" | grep -q "2 objects" || { echo "FAIL: expected exactly 2 objects in the image"; exit 1; }

echo "== 4/5 negative control: lib WITHOUT helper must FAIL the link =="
"$LIBRARIAN_EXE" /CREATE NOHELP.OLB MOD_B.o >/dev/null
if "$LINK_EXE" --shareable --symbol-vector "entry=PROCEDURE" \
        --gsmatch LEQUAL,1,0 -o BAD.EXE PROG.o NOHELP.OLB >/dev/null 2>&1; then
    echo "FAIL: link succeeded against a library that cannot resolve 'helper'"; exit 1
fi
echo "   OK: link correctly failed (unresolved external)"

echo "== 5/5 image validity: OVMXDUMP parses the produced shareable =="
DUMP=$("$OVMXDUMP" FOO.EXE)
echo "$DUMP" | grep -q "symbol vector" || { echo "FAIL: OVMXDUMP found no symbol vector"; exit 1; }
echo "$DUMP" | grep -qE '\] PROCEDURE .* entry' || { echo "FAIL: 'entry' universal missing"; exit 1; }

echo "PASS: LIBRARIAN -> .OLB -> LINK selective member extraction round-trip"
exit 0
