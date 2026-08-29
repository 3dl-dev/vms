#!/bin/bash
# TEST: SHOW ERROR fabricates no error report when it cannot read the executive
#
# WHAT THIS GATES, AND WHY IT ASSERTS AN ABSENCE (vms-050).
#
# SHOW ERROR used to ignore the system entirely. Its whole body was:
#     printf("\n         Device Error Count Summary\n");
#     printf("         Device   Error Count\n");
#     printf("         ------   -----------\n");
#     printf("No errors logged.\n");
# -- a fixed banner ending in "No errors logged.", printed WHATEVER the real
# per-device error counts were. A node whose disk or port device HAD logged
# errors still reported "No errors logged." The command read nothing; it was a
# constant, the exact report-success-while-reading-nothing defect CLAUDE.md
# Rule 11 / INV-6 exist to kill.
#
# SHOW ERROR is now a READER of the executive I/O database: it walks the device
# table with vms_kif_devscan() -- the SAME scan SHOW DEVICE uses and the SAME
# errcnt field F$GETDVI(...,"ERRCNT") reads -- and lists only the devices whose
# error count is greater than zero (VMS's own HELP SHOW ERROR: "Displays the
# error count for all devices with error counts greater than zero"; format +
# filtering captured in docs/oracle/vax73-show-error.md). When the executive
# cannot be reached -- as under ctest, which runs on a host with no /dev/vms and
# never will have one (the only OVMX runtime is the kernel/QEMU path, Rule 9) --
# the scan fails and SHOW ERROR prints NOTHING, $STATUS carrying the failure. It
# never invents an "everything is fine" banner.
#
# WHAT CAN BE PROVEN HERE, AND ONLY HERE: with no executive to read, SHOW ERROR
# emits none of the fabricated markers ("No errors logged.", the "Device Error
# Count Summary" title, the "------" separator rule). The ALIVE marker after it
# proves DCL actually ran the command (so the absence is a real empty result,
# not a dead shell) -- without it an all-EXPECT_NOT gate would pass vacuously.
# Restore ANY line of the deleted banner and a marker reappears: revert the body
# to its old "No errors logged." and the EXPECT_NOT trips -- the gate goes RED.
# (Verified by that mutation while writing this test.) These are therefore
# discriminating assertions about cmd_show_error()'s own code, not decoration.
#
# The POSITIVE proof -- SHOW ERROR reading the REAL device table against a real
# /dev/vms, listing no device because every real errcnt is zero yet still
# printing the report header (so the scan demonstrably ran), and correctly
# OMITTING the real zero-error console OPA0: that SHOW DEVICE lists -- runs in
# the DCL/SHOW acceptance battery (tests/qemu/lib/dcl_acceptance_battery.sh, the
# SHOW ERROR block). One claims the code has no invented-banner branch left; the
# other claims a VMS state read from the executive.
#
# EXPECT: contains:SHOWERR-ALIVE-MARKER
# EXPECT_NOT: contains:No errors logged.
# EXPECT_NOT: contains:Device Error Count Summary
# EXPECT_NOT: contains:-----------
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SHOW ERROR\nWRITE SYS$OUTPUT "SHOWERR-ALIVE-MARKER"\n' | $VMSDCL 2>&1
