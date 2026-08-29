#!/bin/sh
# run_evax_linkgp.sh — integration test for LINK.EXE's per-image linkage-section
# base + per-procedure K computation (bead vms-fd5, component C1 of the vms-5f5
# authentic OpenVMS-Alpha per-image GP program; see
# docs/design-alpha-per-image-gp.md §1.4/§2.3).
#
# Links the synthetic TWO-procedure EVAX fixture linkgp_two_proc.obj (FIRST_PROC
# and SECOND_PROC, both PROCEDUREs, so both get a PDSC placed in the module's
# single $LINK$ psect -- FIRST_PROC's first, SECOND_PROC's second at a nonzero
# offset) into a shareable, with OVMX_LINK_DUMP_GP=1 so LINK.EXE prints its
# %LINK-I-GPBASE / %LINK-I-GPENTRY diagnostic lines recording the linkage-section
# base and each procedure's K = &PDSC - base. evax_linkgp_verify then derives the
# SAME base/K values independently from the emitted image ($LINK$ section
# address + the .vms$sv universal values) and asserts LINK.EXE's recorded state
# matches exactly -- for BOTH procedures, so a K hard-coded to 0 cannot pass
# (SECOND_PROC's ground-truth K is provably nonzero).
#
# Architecture-independent: LINK.EXE's EVAX path + the verifier are pure byte/
# text manipulation, so this runs on the CI host with no Alpha toolchain. The
# .obj fixture is checked in; regenerate with the alpha-dec-vms binutils if
# linkgp_two_proc.s changes:
#   alpha-dec-vms-as -o linkgp_two_proc.obj linkgp_two_proc.s
# Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
FIX="$HERE/evax-fixtures"
WORK=${WORK:-/tmp/evax-linkgp-test}
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== build LINK.EXE (host tool, ELF + EVAX paths) =="
# evax_read.c is #included by link.c (single TU) -- do NOT also compile it here.
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c"

echo "== build the linkage-base/K verifier =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" \
    -o "$WORK/evax_linkgp_verify" "$HERE/evax_linkgp_verify.c"

echo
echo "== emit a 2-procedure Alpha .vms\$sv shareable, dumping LINK.EXE's recorded" \
     "per-image linkage-section base + per-procedure K =="
OVMX_LINK_DUMP_GP=1 "$WORK/LINK.EXE" --shareable \
    --symbol-vector "FIRST_PROC=PROCEDURE,SECOND_PROC=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$WORK/GPTEST\$SHR.EXE" "$FIX/linkgp_two_proc.obj" \
    > "$WORK/link.log" 2>&1
cat "$WORK/link.log"

echo
echo "== verify: recorded base/K match the image's OWN \$LINK\$ base + .vms\$sv values =="
"$WORK/evax_linkgp_verify" "$WORK/GPTEST\$SHR.EXE" "$WORK/link.log"

echo
echo "ALL EVAX LINKAGE-SECTION-BASE / PER-PROCEDURE K (vms-fd5) INTEGRATION CHECKS PASSED"
