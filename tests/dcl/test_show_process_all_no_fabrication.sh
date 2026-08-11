#!/bin/bash
# TEST: SHOW PROCESS/ALL fabricates no process table when it cannot read the
#       executive, and is not a "Process Name / PID / UIC / State" table
#
# WHAT THIS GATES, AND WHY IT ASSERTS ABSENCES (vms-70eb).
#
# The /ALL branch of SHOW PROCESS used to print a "      Processes at <date>"
# banner and a "Process Name / PID / UIC / State" table whose one row was
# built from ctx->process_name -- the DCL context's SELF-DECLARED name, not
# the executive's prcnam -- and a HARDCODED "LEF" scheduler state. The oracle
# capture in docs/oracle/vax73-show-system-process.md Section 4 is decisive on
# both counts: VMS `SHOW PROCESS/ALL` means "all information about THIS
# process" -- the plain display followed by the per-process sections -- and is
# NOT a process table. Section 4 names the OVMX table "the wrong shape AND a
# fabrication".
#
# /ALL now falls through to the same $GETJPI-sourced target selection and
# plain display every other SHOW PROCESS form uses (CLAUDE.md Rule 11: a VMS
# command is a READER of an executive facility, never a fabricator), then
# appends the two privilege blocks it can source from the executive-held mask.
#
# ctest runs on a host with no /dev/vms and never will have one: the only OVMX
# runtime is the kernel/QEMU path (Rule 9). So the POSITIVE proof -- that /ALL
# prints the plain display and the two privilege blocks against a real
# executive -- lives in tests/qemu/test_syssvc_showproc.c (block P6a).
#
# What CAN be proven here, and only here, is the thing that used to be wrong:
# with no executive to read, the $GETJPI in the shared display path fails and
# the command reports that condition and prints NOTHING else -- no "Processes
# at" banner, no "Process Name" table heading, no "LEF" state. On origin/main
# this test is RED: the /ALL branch prints the banner and heading BEFORE it
# calls $GETJPI, so they appear even with no executive, and no %SYSTEM
# condition is printed at all. Restoring the deleted table turns it red again,
# so this is a discriminating assertion about dcl_cmd_show.c's own source, not
# a decoration. It is consistent with the honest-degradation gate in
# tests/dcl/test_show_process.sh, which makes the same claim for plain SHOW
# PROCESS.
#
# EXPECT: regex:%SYSTEM-[A-Z]-
# EXPECT_NOT: contains:Processes at
# EXPECT_NOT: contains:Process Name
# EXPECT_NOT: contains:LEF
# EXPECT_NOT: contains:_FTA0:
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW PROCESS/ALL" | $VMSDCL 2>&1
