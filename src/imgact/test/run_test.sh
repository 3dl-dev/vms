#!/bin/sh
# run_test.sh — IMGACT.EXE proof harness (bead vms-913.2).
#
# Runs INSIDE an Alpine (native musl) container. Builds IMGACT.EXE and the
# proof artifacts, installs them at their OpenVMS paths, then proves the
# activation mechanism end to end plus the missing-image failure path.
#
# Exit 0 only if: (a) the activated test executable prints PASS and exits 0,
# and (b) removing the shareable image yields %IMGACT-F-IMGNOTFND + nonzero.

set -e

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
LIB='LIBTEST$SHR.EXE'
INTERP="$SYSEXE/IMGACT.EXE"

SRC=$(cd "$(dirname "$0")/.." && pwd)   # src/imgact
cd "$SRC"

echo "== toolchain =="
$CC --version | head -1
echo

echo "== build IMGACT.EXE =="
make CC="${CC:-gcc}" clean >/dev/null 2>&1 || true
make CC="${CC:-gcc}"
mkdir -p "$SYSEXE" "$SYSLIB"
cp IMGACT.EXE "$INTERP"
echo "IMGACT.EXE size: $(stat -c %s "$INTERP") bytes"
echo "IMGACT.EXE own dynamic relocations (must be RELATIVE only):"
readelf -rW IMGACT.EXE | awk '/R_AARCH64/{print $3}' | sort | uniq -c || true
echo

echo "== build test shareable image ($LIB) =="
$CC -std=gnu11 -O2 -Wall -shared -fPIC -mtls-dialect=desc -nostdlib \
    -Wl,--hash-style=sysv -Wl,-z,norelro -Wl,-soname,"$LIB" \
    -o "$LIB" test/test_lib.c
echo "$LIB dynamic relocations:"
readelf -rW "$LIB" | awk '/R_AARCH64/{print $3}' | sort | uniq -c
cp "$LIB" "$SYSLIB/$LIB"
echo

echo "== build test executable (PT_INTERP=$INTERP, DT_NEEDED=$LIB) =="
$CC -std=gnu11 -O2 -Wall -no-pie -nostdlib -ffreestanding -fno-stack-protector \
    -Wl,--dynamic-linker="$INTERP" -Wl,--hash-style=sysv -Wl,-z,norelro \
    -Wl,--allow-shlib-undefined -Wl,-e,_start \
    -o test/test_prog test/test_prog.c -L. -l:"$LIB"
echo "test_prog headers:"
readelf -hl test/test_prog | grep -E "Type:|interpreter"
echo "test_prog DT_NEEDED:"
readelf -d test/test_prog | grep NEEDED
echo "test_prog relocations:"
readelf -rW test/test_prog | awk '/R_AARCH64/{print $3}' | sort | uniq -c
echo

echo "== SUCCESS PATH: activate test_prog =="
set +e
OUT=$(./test/test_prog); RC=$?
set -e
echo "stdout/stderr: $OUT"
echo "exit code: $RC"
case "$OUT" in
	*"IMGACT-TEST: PASS"*) ;;
	*) echo "FAILURE: expected PASS line"; exit 1 ;;
esac
[ "$RC" -eq 0 ] || { echo "FAILURE: expected exit 0"; exit 1; }
echo "success path OK"
echo

echo "== FAILURE PATH: remove shareable image, re-activate =="
rm -f "$SYSLIB/$LIB"
set +e
FOUT=$(./test/test_prog 2>&1); FRC=$?
set -e
echo "diagnostic: $FOUT"
echo "exit code: $FRC"
case "$FOUT" in
	*"%IMGACT-F-IMGNOTFND"*) ;;
	*) echo "FAILURE: expected %IMGACT-F-IMGNOTFND"; exit 1 ;;
esac
[ "$FRC" -ne 0 ] || { echo "FAILURE: expected nonzero exit"; exit 1; }
echo "failure path OK"
echo

echo "ALL IMGACT.EXE PROOF CHECKS PASSED"
