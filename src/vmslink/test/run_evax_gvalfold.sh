#!/bin/sh
# run_evax_gvalfold.sh — integration test for LINK.EXE's EVAX/Alpha GLOBALVALUE
# FOLD in the cross-image path (bead vms-069).
#
# A VMS globalvalue is an ABSOLUTE LINK-TIME CONSTANT — its address IS the value.
# VMS resolves it at LINK, folding the constant straight into every reference
# site; it is NEVER bound through an activation import cell (contrast a
# PROCEDURE/DATA universal, which IS an IMGACT-filled .vms$imp import). The ELF
# path already folds these (collect_globalvalues/gval_find, vms-954); this proves
# the EVAX/Alpha cross-image path does too — the case that blocked linking the
# real alpha-dec-vms GCC-port crt0, which references `&C$_EXIT1` where DECC$SHR
# exports C$_EXIT1 as a globalvalue.
#
# Three cases:
#   1. FOLD: link the EVAX fixture gvalfold_ref.obj (`.quad C$_EXIT1` REFQUAD @
#      $DATA$+0, C$_EXIT1 undefined in the object set) against a --use'd producer
#      exporting `C$_EXIT1=GLOBALVALUE:0x0035A009`. LINK.EXE must FOLD the
#      constant into the site (0x0035A009) and emit NO .vms$imp. (verifier)
#   2. PROCEDURE import still works: link link_main.obj against a producer that
#      exports HELPER_PROC=PROCEDURE — must STILL emit a .vms$imp import cell
#      (folding globalvalues must not break procedure imports). (evax_ximport_verify)
#   3. INV-6 honest fail: a symbol exported by NO producer still %LINK-F-UNDEF.
#
# Architecture-independent: the producer is a plain OVMX shareable and LINK.EXE's
# EVAX path + verifiers are pure byte manipulation — no Alpha toolchain needed at
# test time. The EVAX .obj fixtures are checked in (regenerate with the
# alpha-dec-vms binutils if the .s changes). Exit 0 = pass.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
FIX="$HERE/evax-fixtures"
WORK=${WORK:-/tmp/evax-gvalfold-test}
GVAL=0x0035A009
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== build LINK.EXE (host tool, ELF + EVAX paths) =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c"

echo "== build the globalvalue-fold verifier + the cross-image import verifier =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" \
    -o "$WORK/evax_gvalfold_verify" "$HERE/evax_gvalfold_verify.c"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" \
    -o "$WORK/evax_ximport_verify" "$HERE/evax_ximport_verify.c"

echo
echo "== CASE 1: build a DECC\$SHR-like producer exporting C\$_EXIT1 as a GLOBALVALUE =="
cat > "$WORK/dummy.c" <<'EOF'
int dummy(void) { return 0; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/dummy.o" "$WORK/dummy.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "C\$_EXIT1=GLOBALVALUE:$GVAL" \
    --gsmatch LEQUAL,1,0 -o "$WORK/CEXIT\$SHR.EXE" "$WORK/dummy.o"

echo
echo "== link gvalfold_ref.obj --use the globalvalue producer =="
"$WORK/LINK.EXE" --transfer MAIN --use "$WORK/CEXIT\$SHR.EXE" \
    -o "$WORK/ref_gvalfold.exe" "$FIX/gvalfold_ref.obj"

echo
echo "== verify the &C\$_EXIT1 site is FOLDED to $GVAL with NO .vms\$imp =="
"$WORK/evax_gvalfold_verify" "$WORK/ref_gvalfold.exe" "$GVAL"

echo
echo "== CASE 2: a real PROCEDURE cross-image import STILL emits a .vms\$imp cell =="
cat > "$WORK/helper.c" <<'EOF'
int HELPER_PROC(void) { return 42; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/helper.o" "$WORK/helper.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "HELPER_PROC=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$WORK/HELPER\$SHR.EXE" "$WORK/helper.o"
"$WORK/LINK.EXE" --transfer MAIN_PROC --use "$WORK/HELPER\$SHR.EXE" \
    -o "$WORK/main_ximport.exe" "$FIX/link_main.obj"
"$WORK/evax_ximport_verify" "$WORK/main_ximport.exe"
echo "ok:   PROCEDURE export still produces a cross-image import cell (fold did not break imports)"

echo
echo "== CASE 3: INV-6 honest fail — a symbol exported by NO producer still %LINK-F-UNDEF =="
cat > "$WORK/other.c" <<'EOF'
int OTHER_PROC(void) { return 1; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/other.o" "$WORK/other.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "OTHER_PROC=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$WORK/OTHER\$SHR.EXE" "$WORK/other.o"
# C$_EXIT1 is exported by no producer here -> must %LINK-F-UNDEF, never fabricated.
if "$WORK/LINK.EXE" --transfer MAIN --use "$WORK/OTHER\$SHR.EXE" \
        -o "$WORK/should_not_exist.exe" "$FIX/gvalfold_ref.obj" \
        > "$WORK/fail.log" 2>&1; then
    echo "FAIL: link SUCCEEDED against a producer that does not export C\$_EXIT1"
    echo "      (an undefined globalvalue must not be fabricated — INV-6)"
    cat "$WORK/fail.log"
    exit 1
fi
grep -q '%LINK-F-UNDEF' "$WORK/fail.log" \
    || { echo "FAIL: expected %LINK-F-UNDEF, got:"; cat "$WORK/fail.log"; exit 1; }
echo "ok:   C\$_EXIT1-in-no-producer -> %LINK-F-UNDEF (no fabricated fold)"

echo
echo "ALL EVAX GLOBALVALUE-FOLD (vms-069) INTEGRATION CHECKS PASSED"
