#!/bin/sh
# run_vms_std_activation_alpha.sh -- IMGACT VMS-standard activation proof for
# Alpha (bead vms-f60d): the activation-time face of R8 (the Alpha calling
# standard). Runs INSIDE the ovmx-cross-alpha container (alpha-linux-gnu cross
# toolchain + user-mode qemu-alpha), like run_test_alpha.sh.
#
# Proves, with the REAL IMGACT.EXE and the REAL trampoline:
#
#   1. STANDARD-CALL CORRECTNESS (test_std_call_alpha): the six activation
#      args land in R16-R21, R25 == AI (documented layout), R27 == PV, R26
#      (RA) points back into IMGACT (the call RETURNS), and R0 is delivered.
#
#   2. END-TO-END VMS-STANDARD ACTIVATION (img.S, flavor VMS_STD): real IMGACT
#      reads .vms$xfer, standard-calls the stub transfer address (which returns
#      the ODD condition value 0x0BAD), control RETURNS to IMGACT, and the
#      returned value is routed to $EXIT. With no /dev/vms it FAILS HONEST
#      (%IMGACT-F-NOSUCHDEV, exit 45) -- it does NOT fake exit(cond & 1 ? 0 : 1)
#      (which for the odd 0x0BAD would be exit 0). This is the INV-6 proof.
#
#   3. ZERO REGRESSION + the flavor gate (img.S, flavor SYSV): the identical
#      image with flavor SYSV takes the UNCHANGED tail-jump path -- IMGACT jumps
#      to _start (which exits 77), and NEVER prints the standard-call NOSUCHDEV
#      diagnostic. Absence/other-flavor => today's behavior, byte-for-byte.
#
# Exit 0 only if all three hold.

set -e

CC=${CC:-alpha-linux-gnu-gcc}
QEMU=${QEMU:-qemu-alpha}

SRC=$(cd "$(dirname "$0")/.." && pwd)   # src/imgact
FIX="$SRC/test/vmsstd"
WORK=$(mktemp -d)
INTERP="$WORK/IMGACT.EXE"

echo "== toolchain =="
$CC --version | head -1
$QEMU --version | head -1
echo

echo "== build IMGACT.EXE (alpha) =="
cd "$SRC"
make ARCH=alpha CC="$CC" clean >/dev/null 2>&1 || true
make ARCH=alpha CC="$CC" >/dev/null
cp IMGACT.EXE "$INTERP"
echo "IMGACT.EXE: $(stat -c %s "$INTERP") bytes at $INTERP"
echo

echo "== 1. STANDARD-CALL CORRECTNESS (trampoline unit proof) =="
$CC -std=gnu11 -O2 -Wall -Wextra -Iinclude -static \
    -o "$WORK/test_std_call_alpha" \
    test/test_std_call_alpha.c test/std_call_stub_alpha.S \
    arch/alpha/vms_transfer.S
$QEMU "$WORK/test_std_call_alpha"
echo

# interp.S names the run-time IMGACT.EXE path (baked into the image at link).
printf '\t.section .interp, "a"\n\t.asciz "%s"\n' "$INTERP" > "$WORK/interp.S"

build_img() {
	flavor="$1"; out="$2"
	$CC -nostdlib -no-pie -static -DXFER_FLAVOR="$flavor" \
	    -Wl,-T,"$FIX/img.lds" -Wl,-e,_start -Wl,--build-id=none \
	    -o "$out" "$FIX/img.S" "$WORK/interp.S" 2>/dev/null
}

echo "== 2. END-TO-END VMS-STANDARD ACTIVATION (flavor VMS_STD) =="
build_img 1 "$WORK/vmsstd_img"
set +e
OUT=$($QEMU "$WORK/vmsstd_img" 2>&1); RC=$?
set -e
echo "output: $OUT"
echo "exit code: $RC"
case "$OUT" in
	*"%IMGACT-F-NOSUCHDEV"*) ;;
	*) echo "FAIL: expected the standard-call \$EXIT path to fail honest with %IMGACT-F-NOSUCHDEV"; exit 1 ;;
esac
[ "$RC" -eq 45 ] || { echo "FAIL: expected fail-honest exit 45, got $RC"; exit 1; }
[ "$RC" -ne 0 ] || { echo "FAIL: exit 0 would mean a FAKED exit(cond&1) of the odd 0x0BAD"; exit 1; }
echo "VMS_STD activation OK (standard call issued, returned to IMGACT, fail-honest \$EXIT)"
echo

echo "== 3. ZERO REGRESSION + flavor gate (flavor SYSV) =="
build_img 0 "$WORK/sysv_img"
set +e
SOUT=$($QEMU "$WORK/sysv_img" 2>&1); SRC_RC=$?
set -e
echo "output: $SOUT"
echo "exit code: $SRC_RC"
case "$SOUT" in
	*"%IMGACT-F-NOSUCHDEV"*)
		echo "FAIL: SYSV-flavor image must NOT take the standard-call \$EXIT path"; exit 1 ;;
esac
[ "$SRC_RC" -eq 77 ] || { echo "FAIL: SYSV tail-jump should reach _start (exit 77), got $SRC_RC"; exit 1; }
echo "SYSV flavor OK (unchanged tail-jump to _start; no standard call)"
echo

rm -rf "$WORK"
echo "ALL IMGACT VMS-STANDARD ACTIVATION PROOF CHECKS PASSED"
