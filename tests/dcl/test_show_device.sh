#!/bin/bash
# TEST: SHOW DEVICE prints no device row it did not read from the executive
# EXPECT: contains:%MOUNT-I-MOUNTED, TESTDISK mounted on _DUA0:
# EXPECT_NOT: regex:^\$1\$DGA
# EXPECT_NOT: contains:OVMXSYS
# EXPECT_NOT: contains:Error    Volume
# EXPECT_NOT: regex:^[A-Z0-9$_]+: +(Mounted|Dismounted)
#
# WHAT THIS ASSERTS, and what it deliberately does not (vms-fb9).
#
# The invariant under test is the item's own: "no code path prints a device
# row it did not read from /dev/vms". That is a NEGATIVE property, and it is
# checkable anywhere -- including here, where ctest runs and no executive
# exists. Every token below is one the deleted code emitted and the reader
# cannot:
#
#   ^$1$DGA           the invented name given to a Linux mount point
#   OVMXSYS           the invented volume label for "/"
#   "Error    Volume" the 7-column header of the old mount-derived listing;
#                     the oracle's terminal listing is 3 columns
#                     (docs/oracle/vax73-terminal-device.md section 4)
#   ^NAME: Mounted    a row from the process-local table MOUNT keeps in this
#                     process's memory, or the hardcoded stub row that was
#                     printed when even /proc/mounts produced nothing
#
# MOUNT DUA0: runs FIRST on purpose. It populates that process-local table,
# so SHOW DEVICE in the same process would print DUA0: from it if it still
# had that source. The paired EXPECT on MOUNT's own message is what stops
# this test passing vacuously because DCL never started.
#
# ANCHORING (this repo has shipped assertions satisfiable by something other
# than the behaviour under test, so it is spelled out): the EXPECT_NOT
# patterns are line-anchored or contain column runs precisely because MOUNT's
# own success message contains the strings "DUA0:" and "TESTDISK". An
# unanchored `EXPECT_NOT: contains:DUA0:` would be tripped by MOUNT's message
# no matter what SHOW DEVICE did; an unanchored `contains:Mounted` likewise,
# by "mounted on". The old fabricated rows started at column 0 with the
# device name; MOUNT's messages start with "%MOUNT-".
#
# WHAT IS NOT ASSERTED HERE: that SHOW DEVICE shows OPA0: when an executive
# IS present. That needs a real /dev/vms and cannot run under ctest at all
# (CLAUDE.md Rule 9) -- and today it cannot run in the QEMU runtime either,
# because nothing in production calls vms_kif_register() and vms.ko rejects
# ioctls from unregistered processes. That is escalated, not papered over;
# see the note in src/vmsdcl/dcl_cmd_show.c above cmd_show_terminal. This
# file does not pretend to cover it.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'MOUNT DUA0: TESTDISK\nSHOW DEVICE\nSHOW DEVICE DUA0:\n' | $VMSDCL 2>&1
