#!/bin/bash
# TEST: F$GETDVI("device","VOLNAM") must agree with SHOW DEVICE's Volume Label for the
#       same device -- the two interfaces must read the same source of truth
# EXPECT: contains:SHOW_DEVICE_LABEL=OVMXSYS
# EXPECT: contains:GETDVI_DKA0_VOLNAM=OVMXSYS
# EXPECT: contains:GETDVI_SYSDEVICE_VOLNAM=OVMXSYS
# EXPECT: contains:LABELS_AGREE
# EXPECT_NOT: contains:LABELS_DISAGREE
#
# Root cause (vms-b9f R5): SHOW DEVICE (dcl_cmd_show.c) reads vms_device_table and printed
# DKA0:'s volume label as "OVMXSYS" (set at boot, dcl_main.c setup_session()), but
# F$GETDVI("DKA0","VOLNAM") (dcl_lexical.c lex_getdvi()) had its own, completely separate,
# hardcoded heuristic: it only returned "OVMXSYS" when the device string literally contained
# the substring "SYSDEVICE" or was empty, and fell through to the generic literal "VOLUME"
# for any other spelling of the same device -- so F$GETDVI("DKA0","VOLNAM") returned "VOLUME"
# while SHOW DEVICE simultaneously reported "OVMXSYS" for the identical device. No test
# exercised F$GETDVI("DKA0","VOLNAM") before this.
#
# Fix: F$GETDVI's VOLNAM branch now looks up vms_find_device() -- the same table SHOW DEVICE
# reads -- resolving the SYS$SYSDEVICE logical to the boot system device first. This test
# extracts SHOW DEVICE's own printed label for DKA0: and compares it against
# F$GETDVI("DKA0","VOLNAM") and F$GETDVI("SYS$SYSDEVICE","VOLNAM"), rather than hardcoding
# "OVMXSYS" as the expected value on both sides -- so it verifies AGREEMENT between the two
# interfaces, not just that each one independently matches a string this test also authored.
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

output=$(printf 'SHOW DEVICE\nX = F$GETDVI("DKA0","VOLNAM")\nSHOW SYMBOL X\nY = F$GETDVI("SYS$SYSDEVICE","VOLNAM")\nSHOW SYMBOL Y\n' | $VMSDCL 2>&1)
echo "$output"

# Pull SHOW DEVICE's own printed label for DKA0: out of its data row (column 48, per
# tests/dcl/test_show_device_no_leak.sh's oracle-pinned layout) -- this is the ground truth
# both F$GETDVI calls must match, not a value this test invents.
dka0_line=$(echo "$output" | grep '^DKA0:')
show_device_label=$(echo "${dka0_line:48:14}" | sed 's/ *$//')
echo "SHOW_DEVICE_LABEL=$show_device_label"

getdvi_dka0=$(echo "$output" | grep 'X = ' | sed 's/.*X = "\(.*\)"/\1/')
echo "GETDVI_DKA0_VOLNAM=$getdvi_dka0"

getdvi_sysdevice=$(echo "$output" | grep 'Y = ' | sed 's/.*Y = "\(.*\)"/\1/')
echo "GETDVI_SYSDEVICE_VOLNAM=$getdvi_sysdevice"

if [ "$show_device_label" = "$getdvi_dka0" ] && [ "$show_device_label" = "$getdvi_sysdevice" ]; then
    echo "LABELS_AGREE"
else
    echo "LABELS_DISAGREE"
fi
