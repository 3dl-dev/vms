#!/bin/bash
# TEST: SHOW USERS fabricates no process row when it cannot read the executive
#
# COVERAGE CHANGED HERE AGAIN, DELIBERATELY (vms-72c) -- READ THIS BEFORE
# "FIXING" THE REGEX BACK TO REQUIRE A PID.
#
# vms-fb9 (the history section this file used to carry, kept below) got SHOW
# USERS halfway to honest: it stopped inventing a TERMINAL NAME, but it kept
# inventing a ROW -- cmd_show_users() read a file-based terminal-allocation
# table whose only WRITER vms-fb9 had already deleted, so the table was
# permanently empty and the "no entries" branch always ran, unconditionally.
# That branch fabricated a single row out of the CALLING process's own DCL
# context: ctx->username, getpid(), and ctx->terminal.device_name (empty,
# which is why the old EXPECT here only forbade a fabricated NAME, not a
# fabricated ROW -- the row's other fields were still invented, just less
# obviously so). This is why the old assertion (regex:[0-9A-F]{8}, i.e. "a
# PID must appear") is now WRONG rather than merely superseded: it was
# pinning the exact shape of the remaining half of the facade.
#
# SHOW USERS is now a READER of the executive's process table
# (src/kernel/vms_proctab.c, via vms_kif_procscan), the same source
# cmd_show_system() and cmd_show_process() already use (CLAUDE.md Rule 11:
# a user-visible VMS command reads an executive facility, it does not
# fabricate its own answer) -- and it lists a row only for a process the
# executive has actually bound to a terminal (VMS_IOCTL_SETTERM, vms-d0b).
#
# ctest runs on a host with no /dev/vms and never will have one: the only
# OVMX runtime is the kernel/QEMU path (Rule 9), exactly the reasoning
# test_show_system_no_fabrication.sh (this directory) already states for
# SHOW SYSTEM. So the positive proof -- SHOW USERS naming a REAL logged-in
# session by its REAL executive-assigned VMS PID and its REAL terminal --
# lives in tests/uat/vms_session_qemu.sh, against a real executive, a real
# console login and a real bound terminal. What CAN be proven here, and
# only here, is the thing that used to be wrong: with no executive to read
# (and so no process this invocation could possibly have bound a terminal
# for), SHOW USERS prints its banner, its header, and NO ROWS AT ALL.
# Restore the deleted per-process fallback and a row reappears with this
# invocation's own getpid() in it -- so this is a discriminating assertion
# about dcl_cmd_show.c's own code, not a decoration.
#
# EXPECT: contains:Username
# EXPECT: contains:Total number of users
# EXPECT_NOT: contains:Node
# EXPECT_NOT: contains:_FTA
# EXPECT_NOT: regex:_[A-Z]{2,3}[0-9]+:
# EXPECT_NOT: regex:[0-9A-F]{8}
#
# --- history (vms-fb9, superseded by the above) -----------------------
# This file used to read:
#   # TEST: SHOW USERS displays VMS terminal device names and process info
#   # EXPECT: regex:_[A-Z]{2,3}[0-9]+:
# and it passed because SHOW USERS printed
#   ctx->terminal.device_name[0] ? ctx->terminal.device_name : "_FTA0:"
# -- i.e. on any system where DCL did not know its terminal (which was every
# system, since knowing it required the VMS_TERMINAL environment handoff
# vms-fb9 deleted) it printed the literal "_FTA0:". The assertion was
# therefore satisfied by a hardcoded string, not by a terminal.
VMSDCL="${VMSDCL:-vmsdcl}"
echo 'SHOW USERS' | $VMSDCL 2>&1
