#!/bin/bash
# TEST: SHOW TERMINAL names no terminal it did not read from the executive
# EXPECT: contains:$STATUS = 676
# EXPECT_NOT: contains:Terminal:
# EXPECT_NOT: contains:Device_Type:
# EXPECT_NOT: contains:Terminal Characteristics:
# EXPECT_NOT: contains:Width:
# EXPECT_NOT: contains:Page:
# EXPECT_NOT: contains:_FTA0:
# EXPECT_NOT: contains:_OPA0:
#
# WHAT CHANGED AND WHY (vms-d0b), because this file used to assert the
# OPPOSITE and the change is not a weakening.
#
# It previously required SHOW TERMINAL to print "Terminal:", "Device_Type:",
# "Terminal Characteristics:", Width, Page, and the characteristic names --
# HERE, under ctest, with no /dev/vms anywhere. Every one of those lines came
# out of struct dcl_context's own copy of a terminal: a device name handed
# down in a VMS_TERMINAL environment variable (or, failing that, a compiled-in
# "_FTA0:"), and characteristics this process had set on itself. Nothing
# outside the process wrote any of it and nothing outside the process could
# see it. The test passed because the facade was complete, which is precisely
# why a passing single-process test proves nothing about a system facility
# (CLAUDE.md rule 11).
#
# SHOW TERMINAL is now a READER: it asks the executive which terminal this job
# is on ($GETJPI) and then reads that device's row ($GETDVI). With no
# executive it can read neither, so it prints NOTHING -- an unanswerable
# question gets no answer rather than a plausible one (rule 10). That is the
# property this file now asserts.
#
# THE POSITIVE ANCHOR IS $STATUS, and it is what stops the EXPECT_NOTs passing
# vacuously -- they would all be satisfied by DCL never starting, or by SHOW
# TERMINAL not being a command at all. With no /dev/vms, vms_kif_open() fails,
# the $GETJPI ioctl fails on the resulting bad descriptor, and
# src/libvmssys/vms_kif.c's vms_kif_kerr_to_ss() maps that -- through its
# closed, oracle-pinned errno set -- to SS$_BUGCHECK (676), which
# cmd_show_terminal returns as $STATUS. 676 can only appear here if SHOW
# TERMINAL actually ran and actually tried to read the executive. Measured:
# `printf 'SHOW SYMBOL $STATUS\n' | DCL.EXE` alone reports 1, not 676.
#
# WHAT IS NOT ASSERTED HERE: what SHOW TERMINAL prints when an executive IS
# present. That needs a real /dev/vms and cannot run under ctest at all
# (CLAUDE.md Rule 9). It is proven in the QEMU runtime instead --
# tests/qemu/test_syssvc_showterm.c drives this same DCL binary against a real
# executive, asserts five rows of the OpenVMS VAX V7.3 capture byte for byte,
# and includes the A-writes/B-reads case where another process changes the
# console's width and characteristics and SHOW TERMINAL reports the change.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SHOW TERMINAL\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
