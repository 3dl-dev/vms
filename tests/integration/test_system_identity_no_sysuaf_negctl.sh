#!/bin/sh
#
# test_system_identity_no_sysuaf_negctl.sh - negative controls for the
# vms-a17e gate (test_system_identity_no_sysuaf.sh).
#
# Same discipline as tests/integration/test_terminal_identity_negctl.sh:
# every property gets its OWN MINIMAL mutation, tripping THAT property and no
# other, so a gate nobody has tried to evade does not sit there certifying
# nothing.
#
# Usage: test_system_identity_no_sysuaf_negctl.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
GATE="$SRC_ROOT/tests/integration/test_system_identity_no_sysuaf.sh"
status=0
passed=0
failed=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "vms-a17e gate negative controls: every property must have an evasion that trips it"

ROOT="$WORK/tree"
mkdir -p "$ROOT"
cp -a "$SRC_ROOT/src" "$ROOT/src"

PROVISION_C="$ROOT/src/ovmx_provision/ovmx_provision.c"
INTERNAL_H="$ROOT/src/kernel/vms_internal.h"
PROCTAB_C="$ROOT/src/kernel/vms_proctab.c"
IOCTL_H="$ROOT/src/kernel/vms_ioctl.h"
cp "$PROVISION_C" "$WORK/ovmx_provision.c.orig"
cp "$INTERNAL_H" "$WORK/vms_internal.h.orig"
cp "$PROCTAB_C" "$WORK/vms_proctab.c.orig"
cp "$IOCTL_H" "$WORK/vms_ioctl.h.orig"

restore() {
    cp "$WORK/ovmx_provision.c.orig" "$PROVISION_C"
    cp "$WORK/vms_internal.h.orig" "$INTERNAL_H"
    cp "$WORK/vms_proctab.c.orig" "$PROCTAB_C"
    cp "$WORK/vms_ioctl.h.orig" "$IOCTL_H"
}

R_NOLOOKUP='PROVISION.EXE does not call sysuaf_lookup() (the SYSTEM-record read)'
R_NOSETIDENT='PROVISION.EXE does not call vms_kif_setident() (LOGINOUT'"'"'s ioctl)'
R_ESTABLISH='PROVISION.EXE calls vms_kif_establish_system() to become SYSTEM'
R_READBACK='PROVISION.EXE still reads its identity back via $GETJPI(self)'
R_KUIC='vms.ko defines the SYSTEM UIC as its own constant'
R_KPRIV='vms.ko defines the SYSTEM privilege mask as its own constant'
R_KHANDLER='vms.ko has an ioctl handler that constructs the SYSTEM identity'
R_STRUCT='struct vms_establish_system_args carries a caller-supplied identity field'

# ---------------------------------------------------------------------------
# POSITIVE CONTROL.
# ---------------------------------------------------------------------------
out=$(sh "$GATE" "$ROOT" 2>&1)
if [ $? -eq 0 ]; then
    echo "  PASS: positive control - unmutated sandbox passes the gate"
    passed=$((passed + 1))
else
    echo "  FAIL: positive control - unmutated sandbox FAILS the gate, so no"
    echo "        negative control below can attribute a RED to its mutation"
    printf '%s\n' "$out" | sed 's/^/          /'
    failed=$((failed + 1))
    status=1
fi

expect_red() {
    name="$1"; need="$2"; shift 2
    out=$(sh "$GATE" "$ROOT" 2>&1)
    rc=$?
    ok=1
    why=""

    if [ "$rc" -eq 0 ]; then
        ok=0
        why="$why
        gate exited 0 -- the evasion was CERTIFIED, not caught"
    fi
    if ! printf '%s\n' "$out" | grep -qF "FAIL: $need"; then
        ok=0
        why="$why
        expected reason not reported: $need"
    fi
    for bad in "$@"; do
        if printf '%s\n' "$out" | grep -qF "FAIL: $bad"; then
            ok=0
            why="$why
        NOT minimal -- also tripped an unrelated property: $bad"
        fi
    done

    if [ "$ok" -eq 1 ]; then
        echo "  PASS: $name"
        passed=$((passed + 1))
    else
        echo "  FAIL: $name$why"
        printf '%s\n' "$out" | sed 's/^/          | /'
        failed=$((failed + 1))
        status=1
    fi
    restore
}

# --- A. The exact regression: PROVISION.EXE reads SYSUAF for SYSTEM again
printf '%s\n' 'static void evade(void) { sysuaf_record_t r; sysuaf_lookup("SYSTEM", &r); }' >> "$PROVISION_C"
expect_red "A: sysuaf_lookup(\"SYSTEM\") reintroduced in PROVISION.EXE" \
    "$R_NOLOOKUP" "$R_NOSETIDENT" "$R_ESTABLISH" "$R_READBACK" \
    "$R_KUIC" "$R_KPRIV" "$R_KHANDLER" "$R_STRUCT"

# --- B. ...or hands a SYSUAF-derived value to the old ioctl --------------
printf '%s\n' 'static void evade(void) { vms_kif_setident("SYSTEM", 0, 0); }' >> "$PROVISION_C"
expect_red "B: vms_kif_setident() reintroduced in PROVISION.EXE" \
    "$R_NOSETIDENT" "$R_NOLOOKUP" "$R_ESTABLISH" "$R_READBACK" \
    "$R_KUIC" "$R_KPRIV" "$R_KHANDLER" "$R_STRUCT"

# --- C. Identity establishment deleted outright (not just SYSUAF-fed) ----
# Deleting the call satisfies the absence checks, which is why C and D exist
# as the paired positives.
sed -i.bak '/vms_kif_establish_system()/d' "$PROVISION_C"
expect_red "C: vms_kif_establish_system() call deleted from PROVISION.EXE" \
    "$R_ESTABLISH" "$R_NOLOOKUP" "$R_NOSETIDENT" "$R_READBACK" \
    "$R_KUIC" "$R_KPRIV" "$R_KHANDLER" "$R_STRUCT"

# --- D. The readback deleted (identity established, never reported) -----
sed -i.bak '/vms_kif_getjpi_self(&info)/d' "$PROVISION_C"
expect_red "D: vms_kif_getjpi_self() readback deleted from PROVISION.EXE" \
    "$R_READBACK" "$R_NOLOOKUP" "$R_NOSETIDENT" "$R_ESTABLISH" \
    "$R_KUIC" "$R_KPRIV" "$R_KHANDLER" "$R_STRUCT"

# --- E. The executive stops owning the UIC constant ----------------------
# The replacement must not CONTAIN "VMS_SYSTEM_UIC" as a substring (grep -F
# would still match it and the mutation would prove nothing).
sed -i.bak 's/VMS_SYSTEM_UIC/SYS_UIC_CONST_RENAMED/g' "$INTERNAL_H"
expect_red "E: VMS_SYSTEM_UIC constant renamed out of vms_internal.h" \
    "$R_KUIC" "$R_NOLOOKUP" "$R_NOSETIDENT" "$R_ESTABLISH" "$R_READBACK" \
    "$R_KPRIV" "$R_KHANDLER" "$R_STRUCT"

# --- F. The executive stops owning the privilege-mask constant -----------
sed -i.bak 's/VMS_PRV_M_SYSTEM_ALL/SYS_PRIV_CONST_RENAMED/g' "$INTERNAL_H"
expect_red "F: VMS_PRV_M_SYSTEM_ALL constant renamed out of vms_internal.h" \
    "$R_KPRIV" "$R_NOLOOKUP" "$R_NOSETIDENT" "$R_ESTABLISH" "$R_READBACK" \
    "$R_KUIC" "$R_KHANDLER" "$R_STRUCT"

# --- G. The executive's ioctl handler deleted -----------------------------
sed -i.bak 's/vms_ioctl_establish_system/vms_ioctl_construct_identity/g' "$PROCTAB_C"
expect_red "G: vms_ioctl_establish_system() renamed out of vms_proctab.c" \
    "$R_KHANDLER" "$R_NOLOOKUP" "$R_NOSETIDENT" "$R_ESTABLISH" "$R_READBACK" \
    "$R_KUIC" "$R_KPRIV" "$R_STRUCT"

# --- H. The ioctl struct grows a caller-supplied identity field ----------
# The absence checks A/B and the presence checks C/D/E/F/G are all
# satisfiable while still letting a caller SUPPLY the identity through the
# args struct, which is the exact shape VMS_IOCTL_SETIDENT has -- this is the
# check that the new ioctl is not just a renamed setident.
sed -i.bak 's/struct vms_establish_system_args {/struct vms_establish_system_args {\n    uint32_t uic;/' "$IOCTL_H"
expect_red "H: uic field added to struct vms_establish_system_args" \
    "$R_STRUCT" "$R_NOLOOKUP" "$R_NOSETIDENT" "$R_ESTABLISH" "$R_READBACK" \
    "$R_KUIC" "$R_KPRIV" "$R_KHANDLER"

echo ""
echo "=========================================="
echo "  RESULTS: $passed passed, $failed failed"
echo "=========================================="
exit $status
