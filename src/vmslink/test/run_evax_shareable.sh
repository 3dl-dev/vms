#!/bin/sh
# run_evax_shareable.sh — integration test for LINK.EXE's EVAX/Alpha SHAREABLE
# emit (bead vms-c65). Builds the real LINK.EXE (host tool), links the checked-in
# alpha-dec-vms producer fixture shr_lib.obj into an Alpha .vms$sv shareable
# (FOO$SHR.EXE), and verifies it is a genuine, IMGACT-consumable symbol-vector
# producer: EM_ALPHA ET_DYN with a .vms$sv (magic + 2 universals FOO/BAR bound to
# their addresses + GSMATCH LEQUAL/1/0), a .vms$rel for the BAR->FOO data pointer,
# NO .vms$xfer, and NO PT_INTERP. The .vms$sv is parsed with the SAME reader
# (ovmx_symvec.h) IMGACT and the ELF shareable path use — the byte-compatibility
# proof (an Alpha and an x86_64 shareable differ only in e_machine + .text arch).
#
# Then the ROUND-TRIP that matters: link the EVAX consumer shr_consumer.obj ALONE
# (its FOO target absent from the object set) against FOO$SHR.EXE via `--use`.
# LINK.EXE must bind FOO as a cross-image import by symbol-vector INDEX (the same
# vms-c179 machinery), proving an Alpha shareable produced by emit_evax_shareable
# is CONSUMABLE. Finally the INV-6 honest-fail: a universal naming an internal
# symbol no input object defines must %LINK-F-NOUNIV, never a bogus vector entry.
#
# Architecture-independent: LINK.EXE's EVAX path + the verifier are pure byte
# manipulation, so this runs on the CI host with no Alpha toolchain. The .obj
# fixtures are checked in; regenerate with the alpha-dec-vms binutils if the .s
# files change (see the header of each .s). Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
FIX="$HERE/evax-fixtures"
WORK=${WORK:-/tmp/evax-shareable-test}
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== build LINK.EXE (host tool, ELF + EVAX paths) =="
# evax_read.c is #included by link.c (single TU) — do NOT also compile it here.
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c"

echo "== build the EVAX shareable verifier =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" \
    -o "$WORK/evax_shareable_verify" "$HERE/evax_shareable_verify.c"

echo "== build the cross-image import verifier (round-trip) =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" \
    -o "$WORK/evax_ximport_verify" "$HERE/evax_ximport_verify.c"

echo
echo "== emit an Alpha .vms\$sv shareable from an alpha-dec-vms object =="
"$WORK/LINK.EXE" --shareable --symbol-vector "FOO=PROCEDURE,BAR=DATA" \
    --gsmatch LEQUAL,1,0 -o "$WORK/FOO\$SHR.EXE" "$FIX/shr_lib.obj"

echo
echo "== verify the shareable (.vms\$sv format parity + GSMATCH + no .vms\$xfer) =="
"$WORK/evax_shareable_verify" "$WORK/FOO\$SHR.EXE"

echo
echo "== ROUND-TRIP: link an EVAX consumer that imports FOO via --use =="
# No shr_lib.obj: FOO is undefined in the object set and must bind as a
# cross-image import against FOO$SHR.EXE by symbol-vector index.
"$WORK/LINK.EXE" --transfer CONSUMER_PROC --use "$WORK/FOO\$SHR.EXE" \
    -o "$WORK/consumer.exe" "$FIX/shr_consumer.obj" > "$WORK/consumer.log" 2>&1
cat "$WORK/consumer.log"
grep -q "cross-image import 'FOO' bound to --use producer FOO\$SHR.EXE \[sv#0\]" \
    "$WORK/consumer.log" \
    || { echo "FAIL: consumer did not bind FOO to FOO\$SHR.EXE at sv#0"; exit 1; }
echo "ok:   consumer binds FOO to the Alpha shareable FOO\$SHR.EXE at sv#0 (by vector index)"

# The consumer image itself is a valid EVAX image with a .vms$imp naming sv#0.
readelf -S "$WORK/consumer.exe" 2>/dev/null | grep -q '.vms\$imp' \
    || { echo "FAIL: consumer image has no .vms\$imp table"; exit 1; }
echo "ok:   consumer image carries a .vms\$imp table (IMGACT-filled at activation)"

echo
echo "== INV-6 honest fail: a universal naming an undefined internal symbol =="
if "$WORK/LINK.EXE" --shareable --symbol-vector "FOO=PROCEDURE,NOPE=PROCEDURE" \
        --gsmatch LEQUAL,1,0 -o "$WORK/BAD\$SHR.EXE" "$FIX/shr_lib.obj" \
        > "$WORK/bad.log" 2>&1; then
    echo "FAIL: shareable emit SUCCEEDED with a universal (NOPE) no object defines"
    echo "      (an undefined universal must not be fabricated — INV-6)"
    cat "$WORK/bad.log"
    exit 1
fi
grep -q '%LINK-F-NOUNIV' "$WORK/bad.log" \
    || { echo "FAIL: expected %LINK-F-NOUNIV, got:"; cat "$WORK/bad.log"; exit 1; }
echo "ok:   universal with no defining internal symbol -> %LINK-F-NOUNIV (no bogus vector entry)"

echo
echo "ALL EVAX SHAREABLE (vms-c65) INTEGRATION CHECKS PASSED"
