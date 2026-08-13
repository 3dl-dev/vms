#!/bin/sh
#
# test_system_identity_no_sysuaf.sh - standing gate (vms-a17e): PID 1 /
# PROVISION.EXE never reads SYS$SYSTEM:SYSUAF.DAT to construct SYSTEM's
# identity.
#
# WHY THIS GATE EXISTS. On OpenVMS, EXEC_INIT constructs the system
# process's identity; LOGINOUT is SYSUAF's FIRST reader. Before vms-a17e,
# OVMX inverted that: PROVISION.EXE (src/ovmx_provision/ovmx_provision.c)
# read SYSUAF's SYSTEM record with sysuaf_lookup() and handed the values to
# VMS_IOCTL_SETIDENT -- the exact ioctl reserved for LOGINOUT authenticating
# an arbitrary user. vms-a17e moved identity construction into vms.ko itself,
# following the OPA0: precedent already in src/kernel/vms_devtab.c (the
# executive creates the fact itself, from constants it owns, before any
# process asks) -- see VMS_SYSTEM_UIC / VMS_PRV_M_SYSTEM_ALL in
# src/kernel/vms_internal.h and vms_ioctl_establish_system() in
# src/kernel/vms_proctab.c.
#
# Tokens are matched against source with C comments STRIPPED (same method as
# tests/integration/test_terminal_identity.sh), so the prose explaining what
# is now ABSENT does not itself satisfy or trip a check.
#
# tests/qemu/test_release_e2e.sh's "poisoned_uic" case is the BEHAVIOURAL
# proof this static gate cannot be: a SYSUAF SYSTEM row carrying UIC [50,50]
# and PRIVILEGES=NONE still produces "system identity SYSTEM [1,4] ..." on a
# real boot. This file only proves the source no longer contains the code
# path that would have read those fields.
#
# Usage: test_system_identity_no_sysuaf.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
status=0

echo "vms-a17e source gate: PROVISION.EXE constructs no identity from SYSUAF"

# Strip C comments (/* */ and //) -- see test_terminal_identity.sh for the
# same helper and its caveat (string literals containing "/*" do not occur
# in the files scanned here).
strip_comments() {
    awk '
    BEGIN { inc = 0 }
    {
        line = $0; out = ""; i = 1
        while (i <= length(line)) {
            two = substr(line, i, 2)
            if (inc) {
                if (two == "*/") { inc = 0; i += 2 } else { i++ }
            } else if (two == "/*") {
                inc = 1; i += 2
            } else if (two == "//") {
                break
            } else {
                out = out substr(line, i, 1); i++
            }
        }
        print out
    }' "$1"
}

scan_absent() {
    label="$1"; token="$2"; shift 2
    hit=""
    for f in "$@"; do
        [ -f "$f" ] || continue
        if strip_comments "$f" | grep -qF "$token"; then
            hit="$hit $f"
        fi
    done
    if [ -n "$hit" ]; then
        echo "FAIL: $label"
        echo "  -> found '$token' in code:$hit"
        status=1
    else
        echo "  OK: $label"
    fi
}

scan_present() {
    label="$1"; token="$2"; shift 2
    found=0
    for f in "$@"; do
        [ -f "$f" ] || continue
        if strip_comments "$f" | grep -qF "$token"; then
            found=1
        fi
    done
    if [ "$found" -eq 1 ]; then
        echo "  OK: $label"
    else
        echo "FAIL: $label"
        echo "  -> '$token' not found in code:$*"
        status=1
    fi
}

PROVISION_C="$SRC_ROOT/src/ovmx_provision/ovmx_provision.c"
INTERNAL_H="$SRC_ROOT/src/kernel/vms_internal.h"
PROCTAB_C="$SRC_ROOT/src/kernel-core/vms_proctab.c"
IOCTL_H="$SRC_ROOT/src/kernel/vms_ioctl.h"

# --- 1. PROVISION.EXE no longer reads SYSUAF for identity ---------------
scan_absent "PROVISION.EXE does not call sysuaf_lookup() (the SYSTEM-record read)" \
    'sysuaf_lookup(' "$PROVISION_C"
scan_absent "PROVISION.EXE does not call vms_kif_setident() (LOGINOUT's ioctl)" \
    'vms_kif_setident(' "$PROVISION_C"

# --- 2. ...and DOES construct identity through the new, argument-free call
# The absence checks above are satisfiable by deleting identity establishment
# outright. This is the paired positive.
scan_present "PROVISION.EXE calls vms_kif_establish_system() to become SYSTEM" \
    'vms_kif_establish_system()' "$PROVISION_C"
scan_present "PROVISION.EXE still reads its identity back via \$GETJPI(self)" \
    'vms_kif_getjpi_self(' "$PROVISION_C"

# --- 3. The executive owns the SYSTEM identity as its own constants -----
scan_present "vms.ko defines the SYSTEM UIC as its own constant" \
    'VMS_SYSTEM_UIC' "$INTERNAL_H"
scan_present "vms.ko defines the SYSTEM privilege mask as its own constant" \
    'VMS_PRV_M_SYSTEM_ALL' "$INTERNAL_H"
scan_present "vms.ko has an ioctl handler that constructs the SYSTEM identity" \
    'vms_ioctl_establish_system' "$PROCTAB_C"

# --- 4. The ioctl that constructs it takes NO caller-supplied identity --
# struct vms_establish_system_args (vms_ioctl.h) must carry no username, uic,
# or privilege field -- unlike struct vms_ident_args (VMS_IOCTL_SETIDENT),
# which legitimately carries all three for LOGINOUT's use. Scoped to the
# struct's own body so vms_ident_args's fields elsewhere in the same header
# do not satisfy this check.
if [ -f "$IOCTL_H" ]; then
    struct_body=$(strip_comments "$IOCTL_H" |
        awk '/struct vms_establish_system_args \{/ { inf = 1 } inf { print } inf && /};/ { exit }')
    if [ -z "$struct_body" ]; then
        echo "FAIL: struct vms_establish_system_args not found in $IOCTL_H"
        echo "  -> the check below cannot have been evaluated, reported as failed"
        status=1
    elif printf '%s\n' "$struct_body" | grep -qE 'username|uic|authorized_privs'; then
        echo "FAIL: struct vms_establish_system_args carries a caller-supplied identity field"
        echo "  -> it must carry only status/pad; the identity is a kernel constant,"
        echo "     never something userspace hands the executive (see vms_ident_args"
        echo "     for the contrast -- that one legitimately carries all three)."
        status=1
    else
        echo "  OK: struct vms_establish_system_args carries no username/uic/privs field"
    fi
fi

if [ "$status" -eq 0 ]; then
    echo "vms-a17e gate: PASS"
else
    echo "vms-a17e gate: FAIL"
fi
exit $status
