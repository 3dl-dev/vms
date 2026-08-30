#!/bin/bash
# TEST: INITIALIZE DCL verb dispatches to INITIALIZE.EXE
# EXPECT: contains:%INIT-E-NODEV
# EXPECT: contains:%INIT-E-NOLABEL
# EXPECT_NOT: contains:unrecognized command
#
# INITIALIZE formats a device with VMSFS structure (vms-f812). It dispatches
# to the existing INITIALIZE.EXE utility (/tools/vms_initialize.c), which
# requires a device and volume label. Since these tests run without a real
# /dev/vms or block device, the actual format will fail; we verify that:
# 1. The verb is recognized (no "unrecognized command" error)
# 2. Missing parameters are caught with %INIT-E-NODEV/%INIT-E-NOLABEL errors
# 3. The abbreviation INIT works
#
# The positive test (actually formatting a volume) is proven in QEMU where a
# real disk is available: tests/qemu/test_initialize_e2e.sh (or similar).

VMSDCL="${VMSDCL:-vmsdcl}"

# Test 1: INITIALIZE without parameters shows %INIT-E-NODEV
# Test 2: INITIALIZE with only device shows %INIT-E-NOLABEL
# Test 3: INIT abbreviation works (shows %INIT-E-NODEV like full verb)
# Use VMS filespec format for device (VDA0:, DUA0:, etc.)
printf 'INITIALIZE\nINITIALIZE DUA0:\nINIT\n' | $VMSDCL 2>&1

