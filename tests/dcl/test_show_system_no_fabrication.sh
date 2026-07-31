#!/bin/bash
# TEST: SHOW SYSTEM fabricates no process row when it cannot read the executive
#
# WHAT THIS GATES, AND WHY IT ASSERTS AN ABSENCE (vms-8019).
#
# SHOW SYSTEM used to print exactly one row -- the calling process -- built
# from that process's own private PCB, and if the PCB was empty it
# FABRICATED a row from getpid() and the DCL context's self-declared process
# name. It could never see any other process, because there was no shared
# process table to enumerate. It is now a READER of the executive's table
# (src/kernel/vms_proctab.c, via vms_kif_procscan) -- CLAUDE.md Rule 11: a
# user-visible VMS command reads an executive facility, it does not
# fabricate its own answer.
#
# ctest runs on a host with no /dev/vms and never will have one: the only
# OVMX runtime is the kernel/QEMU path (Rule 9). So the positive proof --
# SHOW SYSTEM listing more than the calling process -- lives in
# tests/qemu/test_syssvc_procnam.c (block P7), against a real executive.
#
# What CAN be proven here, and only here, is the thing that used to be
# wrong: with no executive to read, SHOW SYSTEM prints its banner and
# column headings and NO PROCESS ROWS AT ALL. Restore either of the two
# deleted fabrication branches and a row appears -- so this is a
# discriminating assertion about dcl_cmd_show.c's own code, not a
# decoration. It is deliberately NOT an endorsement of running OVMX
# without an executive; it is the statement that when the reader has
# nothing to read, it invents nothing.
#
# WHY THIS IS CONSISTENT WITH DELETING THE HOST lib$getjpi BLOCK, which the
# same change did on the stated ground that a host assertion about a VMS
# service is an assertion about a system OVMX never runs as: the deleted
# block asserted a SUCCESS that VMS could never produce with no executive
# present, whereas this gate asserts an ABSENCE OF FABRICATION, which is a
# property of dcl_cmd_show.c's own source and holds in every environment --
# including the QEMU runtime, where P7/P8 of tests/qemu/test_syssvc_procnam.c
# prove the same function printing REAL rows. One claims a VMS state; the
# other claims the code has no invented-row branch left in it.
#
# A process row begins at COLUMN ZERO with 8 hex digits ("2020021C ..."), which
# is where VMS puts it -- oracle-pinned by vms-6a7,
# docs/oracle/vax73-show-system-process.md Section 1.1. The regex below used to
# be "^ [0-9A-F]{8}" (leading space), matching the row format OVMX printed
# before the geometry was pinned; left unchanged it would have gone on passing
# whether or not a fabricated row appeared, because a restored fabrication
# would now print at column 0 and the anchored space would never match. An
# absence assertion that cannot see the thing it forbids is worse than no
# assertion at all.
#
# EXPECT: contains:Process Name
# EXPECT: contains:Uptime
# EXPECT_NOT: regex:^[0-9A-F]{8}
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW SYSTEM" | $VMSDCL 2>&1
