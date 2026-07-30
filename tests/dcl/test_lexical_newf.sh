#!/bin/bash
# TEST: New lexical functions F$PID, F$DEVICE, F$GETDVI, F$IDENTIFIER, F$CVUI work
# EXPECT: regex:P =
# EXPECT: regex:D =
# EXPECT: contains:SYS$SYSDEVICE
# EXPECT: contains:SYSTEM
# EXPECT: regex:C = 65
# EXPECT_NOT: contains:_OPA0:
# EXPECT_NOT: contains:_FTA0:
#
# ONE ASSERTION CHANGED, AND WHY (vms-fb9). This file used to carry
#   # EXPECT: contains:_OPA0:
# for the F$DEVICE("*") line. That passed because populate_device_list() in
# src/vmsdcl/dcl_lexical.c carried a hardcoded array -- "_OPA0:", "_FTA0:",
# "SYS$SYSDEVICE:" -- unioned with /proc/mounts. F$DEVICE enumerated devices
# that did not exist and could not enumerate one that did, and this assertion
# was satisfied by the literal in that array, not by a device.
#
# F$DEVICE now enumerates the executive's device table ($DEVICE_SCAN over
# src/kernel/vms_devtab.c), so it returns the names the executive actually
# holds -- and nothing at all where there is no executive to ask, as under
# ctest. The assertion is inverted rather than dropped: "_OPA0:" must NOT
# appear, which stays true in BOTH environments, because the executive's
# physical name form for the console is "OPA0:" without the leading
# underscore. Whether real VMS's F$DEVICE returns the underscore form is not
# recorded in anything this work has, so it is not asserted either way
# (CLAUDE.md rule 10 -- do not invent VMS behaviour to test against).
#
# The other four lexicals are untouched and their assertions are unchanged;
# they are what keeps this from becoming a test that passes because DCL
# printed nothing. Note F$GETDVI is still a fabricator (it answers from
# src/vmsdcl/dcl_lexical.c's own idea of a device, which is why
# "SYS$SYSDEVICE" still appears) -- that is reported, not fixed here.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'P = F$PID("")\nSHOW SYMBOL P\nD = F$DEVICE("*")\nSHOW SYMBOL D\nG = F$GETDVI("SYS$SYSDEVICE","DEVNAM")\nSHOW SYMBOL G\nI = F$IDENTIFIER(65540,"NUMBER_TO_NAME")\nSHOW SYMBOL I\nC = F$CVUI(0,8,"A")\nSHOW SYMBOL C\n' | $VMSDCL 2>&1
