#!/bin/sh
# run_test_alpha.sh — IMGACT.EXE Alpha proof harness (bead vms-e11, Alpha[A2],
# epic vms-8954).
#
# The Alpha analogue of run_test_x86_64.sh. Runs INSIDE the ovmx-cross-alpha
# container (tools/cross-alpha/Dockerfile), which has the alpha-linux-gnu
# cross toolchain and qemu-user's qemu-alpha (user-mode Alpha emulation,
# confirmed present -- no full qemu-system-alpha kernel boot is needed to
# prove activation, unlike the libvmssys PID-1 proof in
# tools/cross-alpha/boot-ovmx-qemu.sh).
#
# One real architectural difference from x86_64/aarch64: Alpha has no
# TLSDESC (alpha-linux-gnu-gcc rejects -mtls-dialect=desc/gnu2 outright --
# that flag is simply omitted below). GCC's only TLS model for Alpha is the
# traditional General Dynamic model (a JMP_SLOT call to __tls_get_addr, with
# DTPMOD64/DTPREL64 relocations filling the tls_index it reads), which
# imgact.c and imgact_arch.h implement instead (guarded on
# IMGACT_R_TLS_DTPMOD, so x86_64/aarch64 are unaffected).
#
# Exit 0 only if: (a) the qemu-alpha-activated test executable prints PASS
# and exits 0, and (b) removing the shareable image yields
# %IMGACT-F-IMGNOTFND + nonzero.

set -e

CC=${CC:-alpha-linux-gnu-gcc}
READELF=${READELF:-alpha-linux-gnu-readelf}
QEMU=${QEMU:-qemu-alpha}

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

echo "== build IMGACT.EXE (alpha) =="
make ARCH=alpha CC="$CC" clean >/dev/null 2>&1 || true
make ARCH=alpha CC="$CC"
mkdir -p "$SYSEXE" "$SYSLIB"
cp IMGACT.EXE "$INTERP"
echo "IMGACT.EXE machine: $($READELF -h IMGACT.EXE | awk '/Machine:/{print $2,$3,$4}')"
echo "IMGACT.EXE size: $(stat -c %s "$INTERP") bytes"
echo "IMGACT.EXE own dynamic relocations (must be RELATIVE only):"
$READELF -rW IMGACT.EXE | awk '/R_ALPHA/{print $3}' | sort | uniq -c || true
echo

echo "== build test shareable image ($LIB) =="
# No -mtls-dialect: Alpha has exactly one TLS model (General Dynamic); GCC
# rejects -mtls-dialect=desc/gnu2 (confirmed: "unrecognized command-line
# option") since classic Alpha has no TLSDESC hardware/compiler support.
$CC -std=gnu11 -O2 -Wall -shared -fPIC -nostdlib \
    -Wl,--hash-style=sysv -Wl,-z,norelro -Wl,-soname,"$LIB" \
    -o "$LIB" test/test_lib.c
echo "$LIB dynamic relocations (must include DTPMOD64/DTPREL64, GLOB_DAT, JMP_SLOT, RELATIVE):"
$READELF -rW "$LIB" | awk '/R_ALPHA/{print $3}' | sort | uniq -c
cp "$LIB" "$SYSLIB/$LIB"
echo

echo "== build test executable (PT_INTERP=$INTERP, DT_NEEDED=$LIB) =="
$CC -std=gnu11 -O2 -Wall -no-pie -nostdlib -ffreestanding -fno-stack-protector \
    -Wl,--dynamic-linker="$INTERP" -Wl,--hash-style=sysv -Wl,-z,norelro \
    -Wl,--allow-shlib-undefined -Wl,-e,_start \
    -o test/test_prog_alpha test/test_prog.c -L. -l:"$LIB"
echo "test_prog_alpha headers:"
$READELF -hl test/test_prog_alpha | grep -E "Type:|interpreter"
echo "test_prog_alpha DT_NEEDED:"
$READELF -d test/test_prog_alpha | grep NEEDED
echo "test_prog_alpha relocations:"
$READELF -rW test/test_prog_alpha | awk '/R_ALPHA/{print $3}' | sort | uniq -c
echo

echo "== SUCCESS PATH: activate test_prog_alpha under qemu-alpha =="
set +e
OUT=$($QEMU ./test/test_prog_alpha); RC=$?
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
FOUT=$($QEMU ./test/test_prog_alpha 2>&1); FRC=$?
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

echo "ALL IMGACT.EXE ALPHA PROOF CHECKS PASSED"
