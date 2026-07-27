#!/bin/sh
# run_test_x86_64.sh — IMGACT.EXE x86_64 proof harness (bead vms-913.11).
#
# The x86_64 analogue of run_test.sh. Runs INSIDE a container that has an
# x86_64 cross toolchain (x86_64-linux-gnu-gcc) and qemu-user (qemu-x86_64).
# We build on the aarch64 host with the cross compiler and execute the
# activated x86_64 image under explicit qemu-x86_64 emulation — no binfmt or
# host toolchain install required (project containerized-deps rule).
#
# The cross gcc is used (not clang/zig) specifically because IMGACT's TLS
# contract is TLSDESC (as on aarch64, which forces -mtls-dialect=desc); the
# x86_64 TLSDESC dialect -mtls-dialect=gnu2 is a GCC feature. clang <=17
# rejects it and falls back to the __tls_get_addr GD model.
#
# Exit 0 only if: (a) the qemu-activated test executable prints PASS and exits
# 0, and (b) removing the shareable image yields %IMGACT-F-IMGNOTFND + nonzero.

set -e

CC=${CC:-x86_64-linux-gnu-gcc}
READELF=${READELF:-x86_64-linux-gnu-readelf}
QEMU=${QEMU:-qemu-x86_64}

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
LIB='LIBTEST$SHR.EXE'
INTERP="$SYSEXE/IMGACT.EXE"

SRC=$(cd "$(dirname "$0")/.." && pwd)   # src/imgact
cd "$SRC"

echo "== toolchain =="
$CC --version | head -1
$QEMU --version | head -1
echo

echo "== build IMGACT.EXE (x86_64) =="
make ARCH=x86_64 CC="$CC" clean >/dev/null 2>&1 || true
make ARCH=x86_64 CC="$CC"
mkdir -p "$SYSEXE" "$SYSLIB"
cp IMGACT.EXE "$INTERP"
echo "IMGACT.EXE machine: $($READELF -h IMGACT.EXE | awk '/Machine:/{print $2,$3,$4}')"
echo "IMGACT.EXE size: $(stat -c %s "$INTERP") bytes"
echo "IMGACT.EXE own dynamic relocations (must be RELATIVE only):"
$READELF -rW IMGACT.EXE | awk '/R_X86_64/{print $3}' | sort | uniq -c || true
echo

echo "== build test shareable image ($LIB) =="
$CC -std=gnu11 -O2 -Wall -shared -fPIC -mtls-dialect=gnu2 -nostdlib \
    -Wl,--hash-style=sysv -Wl,-z,norelro -Wl,-soname,"$LIB" \
    -o "$LIB" test/test_lib.c
echo "$LIB dynamic relocations (must include TLSDESC, GLOB_DAT, JUMP_SLOT, RELATIVE):"
$READELF -rW "$LIB" | awk '/R_X86_64/{print $3}' | sort | uniq -c
cp "$LIB" "$SYSLIB/$LIB"
echo

echo "== build test executable (PT_INTERP=$INTERP, DT_NEEDED=$LIB) =="
$CC -std=gnu11 -O2 -Wall -no-pie -nostdlib -ffreestanding -fno-stack-protector \
    -Wl,--dynamic-linker="$INTERP" -Wl,--hash-style=sysv -Wl,-z,norelro \
    -Wl,--allow-shlib-undefined -Wl,-e,_start \
    -o test/test_prog test/test_prog.c -L. -l:"$LIB"
echo "test_prog headers:"
$READELF -hl test/test_prog | grep -E "Type:|interpreter"
echo "test_prog DT_NEEDED:"
$READELF -d test/test_prog | grep NEEDED
echo "test_prog relocations:"
$READELF -rW test/test_prog | awk '/R_X86_64/{print $3}' | sort | uniq -c
echo

echo "== SUCCESS PATH: activate test_prog under qemu-x86_64 =="
set +e
OUT=$($QEMU ./test/test_prog); RC=$?
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
FOUT=$($QEMU ./test/test_prog 2>&1); FRC=$?
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

echo "ALL IMGACT.EXE x86_64 PROOF CHECKS PASSED"
