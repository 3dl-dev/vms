#!/bin/sh
# negctl_vmsabi_dsc64.sh - negative control for test_vmsabi_dsc (vms-7b3).
#
# Breaks the DSC64 recognition in a copy of src/libvms/syssvc/vmsabi_dsc.h
# (ovmx_vmsabi_is_dsc64 never sees MBO=1/MBMO=-1, so every descriptor is read
# as the 32-bit form -- what the VMS-ABI entries would do if they guessed the
# form instead of reading it), rebuilds tests/libvms/test_vmsabi_dsc.c against
# the copy, and requires the test to FAIL on the DSC64 assertions. A test that
# still passes with the detection broken cannot be the proof that it works.
#
# Usage: negctl_vmsabi_dsc64.sh <cc> <repo-root>     (ctest: negctl_vmsabi_dsc64)
set -eu
CC=$1
ROOT=$2
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cp "$ROOT/src/libvms/syssvc/vmsabi_dsc.h" "$T/vmsabi_dsc.h"
sed -i 's|    return mbo == 1 \&\& mbmo == -1;|    return 0 \&\& mbo == 1 \&\& mbmo == -1; /* NEGCTL vmsabi-dsc64-not-detected */|' "$T/vmsabi_dsc.h"
grep -q 'NEGCTL vmsabi-dsc64-not-detected' "$T/vmsabi_dsc.h" || {
    echo "NEGCTL FAIL: the mutation did not land (ovmx_vmsabi_is_dsc64 changed?)"; exit 1; }
"$CC" -std=gnu11 -O1 -I"$T" -o "$T/t" "$ROOT/tests/libvms/test_vmsabi_dsc.c"
if "$T/t" > "$T/out" 2>&1; then
    cat "$T/out"
    echo "NEGCTL FAIL: test_vmsabi_dsc PASSED with DSC64 recognition broken"
    exit 1
fi
for want in 'FAIL: DSC64 form (MBO=1, MBMO=-1) reads as DSC64' \
            'FAIL: both forms read the same address' \
            'FAIL: class-D result lands in the DSC64'; do
    grep -qF "$want" "$T/out" || { cat "$T/out"; echo "NEGCTL FAIL: expected red assertion missing: $want"; exit 1; }
done
# The 32-bit-form assertions must stay green: the mutation trips DSC64 only.
grep -qF 'ok: 32-bit form reads as the 32-bit form' "$T/out" || { cat "$T/out"; echo "NEGCTL FAIL: 32-bit form went red too"; exit 1; }
echo "NEGCTL PASS: test_vmsabi_dsc goes red on the DSC64 assertions with detection broken"
