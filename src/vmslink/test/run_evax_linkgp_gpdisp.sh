#!/bin/sh
# run_evax_linkgp_gpdisp.sh — integration test for the OVMX-labeled
# EVAX_R_OVMX_GPDISP relocation round-trip (bead vms-4ed, component C2 of the
# vms-5f5 authentic OpenVMS-Alpha per-image GP program; see
# docs/design-alpha-per-image-gp.md §2.1/§2.2).
#
# [OVMX] EVAX publishes no GP-displacement relocation, so this whole mechanism is
# an OVMX design choice (Rule 8): a callee's ldah/lda GP-establish pair is marked
# by the new `.ovmx_gpdisp` assembler directive, which emits the OVMX-private
# ETIR__C_OVMX_GPDISP (0xEF01) command; the OVMX linker reads it and patches -K
# (signed-split) into the pair, where K = the enclosing procedure's PDSC offset
# within its module linkage section (consumed from C1's evax_gp_entry table).
#
# The ROUND-TRIP:
#   1. gas EMIT   — the checked-in fixture linkgp_gpdisp.obj was assembled by the
#      PATCHED alpha-dec-vms-as (tools/cross-alpha-vms/patches/0006-vms-4ed-...),
#      so the object CARRIES the ETIR__C_OVMX_GPDISP command for two procedures
#      (FIRST_PROC at K==0, SECOND_PROC at K!=0).
#   2. OVMX READ  — LINK.EXE's evax_read.c recognizes opcode 0xEF01 (an unknown
#      ETIR command is otherwise a hard error, so an un-taught reader FAILS).
#   3. OVMX APPLY — LINK.EXE patches -K into the ldah/lda immediates.
# The verifier then asserts the ACTUAL patched immediates on the image decode to
# exactly -K (HIGH(-K)/LOW(-K)), with K derived INDEPENDENTLY from the image's
# $LINK$ base + .vms$sv values -- FIRST_PROC -> 0/0 (K==0), SECOND_PROC -> a real
# nonzero -K. A stubbed/wrong apply cannot pass (SECOND_PROC's K is provably !=0).
#
# Architecture-independent: LINK.EXE's EVAX path + the verifier are pure byte/
# text manipulation, so this runs on the CI host with no Alpha toolchain. The
# .obj fixture is checked in; regenerate with the PATCHED alpha-dec-vms binutils
# if linkgp_gpdisp.s changes:
#   alpha-dec-vms-as -o linkgp_gpdisp.obj linkgp_gpdisp.s
# Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
FIX="$HERE/evax-fixtures"
WORK=${WORK:-/tmp/evax-linkgp-gpdisp-test}
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== build LINK.EXE (host tool, ELF + EVAX paths) =="
# evax_read.c is #included by link.c (single TU) -- do NOT also compile it here.
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c"

echo "== build the EVAX_R_OVMX_GPDISP round-trip verifier =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" \
    -o "$WORK/evax_linkgp_gpdisp_verify" "$HERE/evax_linkgp_gpdisp_verify.c"

echo
echo "== link the 2-procedure Alpha .vms\$sv shareable, applying EVAX_R_OVMX_GPDISP" \
     "and dumping LINK.EXE's per-relocation diagnostic =="
OVMX_LINK_DUMP_GP=1 "$WORK/LINK.EXE" --shareable \
    --symbol-vector "FIRST_PROC=PROCEDURE,SECOND_PROC=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$WORK/GPDISP\$SHR.EXE" "$FIX/linkgp_gpdisp.obj" \
    > "$WORK/link.log" 2>&1
cat "$WORK/link.log"

echo
echo "== verify: the patched ldah/lda immediates on the image decode to -K =="
"$WORK/evax_linkgp_gpdisp_verify" "$WORK/GPDISP\$SHR.EXE" "$WORK/link.log"

echo
echo "ALL EVAX_R_OVMX_GPDISP (vms-4ed) ROUND-TRIP INTEGRATION CHECKS PASSED"
