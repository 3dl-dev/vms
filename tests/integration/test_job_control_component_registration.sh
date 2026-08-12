#!/bin/sh
#
# test_job_control_component_registration.sh - JOB_CONTROL is created by a
# registered STDRV component, not a hardcoded call from a site file
# (vms-2a9, docs/design-boot-faithful.md sec3.8/sec4.5).
#
# WHAT THIS GATE PROVES, AND HOW.
#
# This is the cheap, always-on half of vms-2a9's proof: a source scan, no
# build and no QEMU boot required, so it runs on every plain `ctest`. It
# fails the build the moment JOB_CONTROL's creation drifts back to a
# hardcoded `@SYS$STARTUP:JOB_CONTROL_STARTUP.COM` inside a site file --
# the exact shape this item replaced, in which the STDRV component database
# (SYS$STARTUP:VMS$VMS.DAT) existed but governed nothing.
#
# The EXPENSIVE, END-TO-END halves are:
#   - tests/qemu/test_job_control_console.sh (vms-8d2, unchanged): boots the
#     real mastered image and proves JOB_CONTROL is a real DETACHED process
#     SHOW SYSTEM lists, created via the (now component-driven) mechanism.
#   - tests/qemu/test_job_control_negctl.sh (vms-2a9, new): boots a SECOND
#     mastered image built from the same tree with JOB_CONTROL's
#     registration line removed, and proves the boot reaches the same
#     landmarks but never creates JOB_CONTROL and never reaches a login
#     prompt -- the negative half no source scan can honestly claim.
#
# A source scan cannot prove a runtime property; it can only prove the code
# shape that would produce the wrong one is absent, and the code shape that
# would produce the right one is present. See test_job_control_ownership.sh
# (vms-8d2) for the sibling gate this one is modeled on.

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
status=0

VMSVMS_DAT="$SRC_ROOT/distro/rootfs/vms/SYS0/SYSCOMMON/SYS\$STARTUP/VMS\$VMS.DAT"
SYSTARTUP="$SRC_ROOT/distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM"
JC_STARTUP="$SRC_ROOT/distro/rootfs/vms/SYS0/SYSCOMMON/SYS\$STARTUP/JOB_CONTROL_STARTUP.COM"

echo "JOB_CONTROL component-registration gate: scanning for the STDRV registration"

# --- 1. VMS$VMS.DAT registers a JOB_CONTROL component ----------------------
if [ ! -f "$VMSVMS_DAT" ]; then
    echo "FAIL: $VMSVMS_DAT not found"
    status=1
else
    # An active (non-comment) line naming JOB_CONTROL_STARTUP.COM, in the
    # "phase-name  procedure-filespec" shape RUN_COMPONENTS parses
    # (STARTUP.COM's own header, sec3.3/sec3.4).
    REG_LINE=$(grep -v '^!' "$VMSVMS_DAT" | grep 'JOB_CONTROL_STARTUP\.COM')
    if [ -z "$REG_LINE" ]; then
        echo "FAIL: $VMSVMS_DAT has no active JOB_CONTROL_STARTUP.COM registration"
        status=1
    else
        echo "PASS: $VMSVMS_DAT registers: $REG_LINE"
        # Exactly the two whitespace-separated fields RUN_COMPONENTS's
        # F$ELEMENT(0," ",...) / F$ELEMENT(1," ",...) parse expects -- a
        # line with extra internal spaces would silently parse as an empty
        # procedure field (F$ELEMENT splits on every delimiter occurrence,
        # it does not collapse runs of them).
        FIELD_COUNT=$(printf '%s\n' "$REG_LINE" | awk '{print NF}')
        if [ "$FIELD_COUNT" -eq 2 ]; then
            echo "PASS: registration line has exactly 2 whitespace-separated fields"
        else
            echo "FAIL: registration line has $FIELD_COUNT fields, RUN_COMPONENTS expects exactly 2"
            status=1
        fi
    fi
fi

# --- 2. SYSTARTUP_VMS.COM no longer hardcodes the call ----------------------
if [ ! -f "$SYSTARTUP" ]; then
    echo "FAIL: $SYSTARTUP not found"
    status=1
else
    if grep -qE '^\$[[:space:]]*@SYS\$STARTUP:JOB_CONTROL_STARTUP\.COM[[:space:]]*$' "$SYSTARTUP"; then
        echo "FAIL: $SYSTARTUP still hardcodes '@SYS\$STARTUP:JOB_CONTROL_STARTUP.COM' -- JOB_CONTROL creation is back off the component-registration mechanism"
        status=1
    else
        echo "PASS: $SYSTARTUP does not hardcode JOB_CONTROL_STARTUP.COM"
    fi
fi

# --- 3. JOB_CONTROL_STARTUP.COM still exists and still RUN/DETACHEs --------
if [ ! -f "$JC_STARTUP" ]; then
    echo "FAIL: $JC_STARTUP not found"
    status=1
else
    if grep -q 'RUN /DETACHED' "$JC_STARTUP" && grep -q 'PROCESS_NAME=JOB_CONTROL' "$JC_STARTUP"; then
        echo "PASS: $JC_STARTUP still creates JOB_CONTROL via RUN/DETACHED/PROCESS_NAME=JOB_CONTROL"
    else
        echo "FAIL: $JC_STARTUP no longer contains the RUN/DETACHED/PROCESS_NAME=JOB_CONTROL shape"
        status=1
    fi
fi

if [ "$status" -eq 0 ]; then
    echo "PASS: JOB_CONTROL is registered as a real STDRV component and created by nothing else"
else
    echo "FAIL: see above"
fi

exit "$status"
