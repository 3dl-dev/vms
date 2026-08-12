#!/bin/sh
#
# test_job_control_ownership.sh - PID 1 does not run the console login loop;
# JOB_CONTROL does (vms-8d2, docs/design-init-scope.md sec2/sec5).
#
# WHAT THIS GATE PROVES, AND HOW.
#
# This is the cheap, always-on half of vms-8d2's proof: a source scan, no
# build and no QEMU boot required, so it runs on every plain `ctest`. It
# fails the build the moment the console login loop drifts back into PID 1
# (src/ovmx_init/ovmx_init.c) -- the exact "wrong component" regression
# design-init-scope.md sec2 named ("Real function, wrong owner").
#
# The EXPENSIVE, END-TO-END half -- boot a real image, prove JOB_CONTROL is
# a real DETACHED process the executive named, distinct from PID 1, and that
# SYSTEM can still log in through it and reach DCL -- is
# tests/qemu/test_job_control_console.sh, gated behind OVMX_QEMU_FULL_E2E
# like the other real-boot gates (parts_demo_e2e, boot_scsnode_hostname_e2e).
# A source scan cannot prove a runtime property; it can only prove the code
# that would produce the wrong one is absent, which is what it does here.
#
# If you are here because this test failed: the console login loop belongs
# in src/ovmx_job_control/ovmx_job_control.c (JOB_CONTROL.EXE), created
# DETACHED by SYS$STARTUP:JOB_CONTROL_STARTUP.COM. Do not add a fork/exec
# loop back into ovmx_init.c, and do not allowlist this gate to pass with one
# there.

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
status=0

INIT_C="$SRC_ROOT/src/ovmx_init/ovmx_init.c"
JC_C="$SRC_ROOT/src/ovmx_job_control/ovmx_job_control.c"
JC_CMAKE="$SRC_ROOT/src/ovmx_job_control/CMakeLists.txt"
VMSVMS_DAT="$SRC_ROOT/distro/rootfs/vms/SYS0/SYSCOMMON/SYS\$STARTUP/VMS\$VMS.DAT"
JC_STARTUP="$SRC_ROOT/distro/rootfs/vms/SYS0/SYSCOMMON/SYS\$STARTUP/JOB_CONTROL_STARTUP.COM"

echo "JOB_CONTROL ownership gate: scanning for the console login loop"

# --- 1. PID 1 contains no login-loop machinery -------------------------
if [ ! -f "$INIT_C" ]; then
    echo "FAIL: $INIT_C not found"
    status=1
else
    # These four symbols are the login loop's own signature: the retry/
    # backoff counter, the two message facilities its diagnostics use, and
    # the direct exec of LOGINOUT.EXE. None may appear in PID 1's source.
    for sig in "consecutive_failures" "STARTUP-F-LOGINFAIL" "OVMX-E-NOLOGIN" \
               "execl(loginout_path"; do
        if grep -qF -- "$sig" "$INIT_C"; then
            echo "FAIL: $INIT_C still contains '$sig' -- the login loop is back in PID 1"
            status=1
        fi
    done
    if [ "$status" -eq 0 ]; then
        echo "PASS: $INIT_C carries no login-loop signature"
    fi
fi

# --- 2. JOB_CONTROL.EXE exists and carries the loop ----------------------
if [ ! -f "$JC_C" ]; then
    echo "FAIL: $JC_C not found -- JOB_CONTROL.EXE has no source"
    status=1
else
    for sig in "consecutive_failures" "execl(loginout_path"; do
        if ! grep -qF -- "$sig" "$JC_C"; then
            echo "FAIL: $JC_C is missing '$sig' -- the login loop did not move here"
            status=1
        fi
    done
    if [ "$status" -eq 0 ]; then
        echo "PASS: $JC_C carries the login loop"
    fi
fi

if [ ! -f "$JC_CMAKE" ] || ! grep -q 'OUTPUT_NAME "JOB_CONTROL"' "$JC_CMAKE"; then
    echo "FAIL: $JC_CMAKE missing or does not build JOB_CONTROL.EXE"
    status=1
else
    echo "PASS: $JC_CMAKE builds JOB_CONTROL.EXE"
fi

# --- 3. JOB_CONTROL is created DETACHED, not forked by PID 1 -------------
# vms-2a9: JOB_CONTROL is now invoked through STARTUP.COM's declarative
# STDRV component-registration mechanism (SYS$STARTUP:VMS$VMS.DAT), not a
# hardcoded call inside a site file -- see tests/integration/
# test_job_control_component_registration.sh (vms-2a9) for the fuller,
# dedicated proof of that mechanism (the registration line's shape, and
# that no site file hardcodes the call). This check keeps vms-8d2's
# original intent -- something in the real startup chain names
# JOB_CONTROL_STARTUP.COM, so JOB_CONTROL is not silently unreachable --
# pointed at the file that is actually true of today.
if [ ! -f "$VMSVMS_DAT" ] || ! grep -q "JOB_CONTROL_STARTUP" "$VMSVMS_DAT"; then
    echo "FAIL: $VMSVMS_DAT does not register JOB_CONTROL_STARTUP.COM"
    status=1
else
    echo "PASS: $VMSVMS_DAT registers JOB_CONTROL_STARTUP.COM"
fi

if [ ! -f "$JC_STARTUP" ]; then
    echo "FAIL: $JC_STARTUP not found"
    status=1
else
    if grep -q "RUN */DETACHED" "$JC_STARTUP" && \
       grep -q "PROCESS_NAME=JOB_CONTROL" "$JC_STARTUP" && \
       grep -q "SYS\$SYSTEM:JOB_CONTROL.EXE" "$JC_STARTUP"; then
        echo "PASS: $JC_STARTUP creates JOB_CONTROL via RUN/DETACHED/PROCESS_NAME"
    else
        echo "FAIL: $JC_STARTUP does not RUN/DETACHED/PROCESS_NAME=JOB_CONTROL SYS\$SYSTEM:JOB_CONTROL.EXE"
        status=1
    fi
fi

if [ "$status" -eq 0 ]; then
    echo "JOB_CONTROL ownership gate: ALL CHECKS PASSED"
else
    echo "JOB_CONTROL ownership gate: FAILED"
fi
exit "$status"
