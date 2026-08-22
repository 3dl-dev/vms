#!/bin/sh
# run_evax_ximport.sh — integration test for LINK.EXE's EVAX/Alpha CROSS-IMAGE
# (SYMG) import binding for DECC$SHR-style imports (bead vms-c179).
#
# Builds the real LINK.EXE (host tool), builds a small DECC$SHR-like producer
# shareable that EXPORTS HELPER_PROC (a .vms$sv universal), then links the EVAX
# fixture link_main.obj ALONE — WITHOUT link_helper.obj, so HELPER_PROC is
# undefined in the object set — against that producer with `--use`. LINK.EXE must
# bind every reference to HELPER_PROC (the STC_LP_PSB LINKAGE pair + two REFQUAD
# data pointers) as a cross-image import: leave each fill slot 0 and emit a
# .vms$imp record naming {producer soname, symbol-vector index}, exactly the
# IMGACT-filled mechanism the ELF path uses. The verifier byte-checks the table.
#
# Then the HONEST-FAIL case (INV-6): linking against a producer that does NOT
# export HELPER_PROC must still %LINK-F-UNDEF — a cross-image import is only
# created for a symbol a real producer exports, never fabricated.
#
# Architecture-independent: the producer is a plain OVMX shareable (any host
# arch; load_producer reads only its .vms$sv), and LINK.EXE's EVAX path + the
# verifier are pure byte manipulation — no Alpha toolchain needed. The .obj
# fixture is the checked-in link_main.obj (see run_evax_link.sh). Exit 0 = pass.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
FIX="$HERE/evax-fixtures"
WORK=${WORK:-/tmp/evax-ximport-test}
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== build LINK.EXE (host tool, ELF + EVAX paths) =="
# evax_read.c is #included by link.c (single TU) — do NOT also compile it here.
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c"

echo "== build the cross-image import verifier =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" \
    -o "$WORK/evax_ximport_verify" "$HERE/evax_ximport_verify.c"

echo
echo "== build a DECC\$SHR-like producer exporting HELPER_PROC =="
cat > "$WORK/helper.c" <<'EOF'
int HELPER_PROC(void) { return 42; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/helper.o" "$WORK/helper.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "HELPER_PROC=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$WORK/HELPER\$SHR.EXE" "$WORK/helper.o"

echo
echo "== link the EVAX fixture (link_main.obj ALONE) against the producer =="
# No link_helper.obj: HELPER_PROC is undefined in the object set and must bind as
# a cross-image import against HELPER$SHR.EXE.
"$WORK/LINK.EXE" --transfer MAIN_PROC --use "$WORK/HELPER\$SHR.EXE" \
    -o "$WORK/main_ximport.exe" "$FIX/link_main.obj"

echo
echo "== verify the cross-image import binding (.vms\$imp + zeroed slots) =="
"$WORK/evax_ximport_verify" "$WORK/main_ximport.exe"

echo
echo "== INV-6 honest fail: a producer that does NOT export HELPER_PROC =="
cat > "$WORK/other.c" <<'EOF'
int OTHER_PROC(void) { return 1; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/other.o" "$WORK/other.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "OTHER_PROC=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$WORK/OTHER\$SHR.EXE" "$WORK/other.o"

# This link MUST fail with %LINK-F-UNDEF — HELPER_PROC is exported by no producer.
if "$WORK/LINK.EXE" --transfer MAIN_PROC --use "$WORK/OTHER\$SHR.EXE" \
        -o "$WORK/should_not_exist.exe" "$FIX/link_main.obj" \
        > "$WORK/fail.log" 2>&1; then
    echo "FAIL: link SUCCEEDED against a producer that does not export HELPER_PROC"
    echo "      (an undefined cross-image reference must not be fabricated — INV-6)"
    cat "$WORK/fail.log"
    exit 1
fi
grep -q '%LINK-F-UNDEF' "$WORK/fail.log" \
    || { echo "FAIL: expected %LINK-F-UNDEF, got:"; cat "$WORK/fail.log"; exit 1; }
echo "ok:   undefined-not-in-producer -> %LINK-F-UNDEF (no fabricated import)"

echo
echo "ALL EVAX CROSS-IMAGE IMPORT (vms-c179) INTEGRATION CHECKS PASSED"
