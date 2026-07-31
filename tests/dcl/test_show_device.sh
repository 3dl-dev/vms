#!/bin/bash
# TEST: SHOW DEVICE prints no device row it did not read from the executive
# EXPECT: contains:%MOUNT-I-MOUNTED, TESTDISK mounted on _DUA0:
# EXPECT: contains:$STATUS = 676
# EXPECT_NOT: regex:^Device +Device +Error
# EXPECT_NOT: contains:NOSUCHDEV
# EXPECT_NOT: regex:^\$1\$DGA
# EXPECT_NOT: contains:OVMXSYS
# EXPECT_NOT: contains:Error    Volume
# EXPECT_NOT: regex:^[A-Z0-9$_]+: +(Mounted|Dismounted)
# EXPECT_NOT: contains:%OVMX-F-EXECDEV
# EXPECT_NOT: contains:%OVMX-F-NODEVTAB
#
# WHAT THIS ASSERTS, and what it deliberately does not (vms-fb9).
#
# The invariant under test is the item's own: "no code path prints a device
# row it did not read from /dev/vms". That is a NEGATIVE property, and it is
# checkable anywhere -- including here, where ctest runs and no executive
# exists.
#
# THE FORMAT-INDEPENDENT CHECK IS THE FIRST EXPECT_NOT, and it is the one
# that matters. `^Device +Device +Error` is the two-line column header, which
# src/vmsdcl/dcl_cmd_show.c emits lazily -- only immediately before the first
# row. With no executive there are no rows, so that header must never appear.
# Unlike the token checks below it does not depend on knowing what the
# fabricated row looked like: measured discrimination is `grep -c '^Device  '`
# = 0 on the clean build and 1 on a build that fabricates a row.
#
# The remaining EXPECT_NOTs are HISTORY, kept because a deleted defect that
# nobody checks for comes back. Each is a token the deleted code emitted and
# the reader cannot:
#
#   ^$1$DGA           the invented name given to a Linux mount point
#   OVMXSYS           the invented volume label for "/"
#   "Error    Volume" the 7-column header of the old mount-derived listing;
#                     the oracle's terminal listing is 3 columns
#                     (docs/oracle/vax73-terminal-device.md section 4)
#   ^NAME: Mounted    a row from the process-local table MOUNT keeps in this
#                     process's memory, or the hardcoded stub row that was
#                     printed when even /proc/mounts produced nothing
#   %OVMX-F-EXECDEV   an invented message a PREVIOUS round of this item gave
#                     "the executive did not answer" -- deleted (vms-fb9 r5)
#                     because that condition is the same per-call
#                     executive-absent facade vms-a35/vms-0ff deleted
#                     product-wide (CLAUDE.md rule 10); see the deleted
#                     show_device_exec_failed() comment in dcl_cmd_show.c
#   %OVMX-F-NODEVTAB  same round's invented message for an empty device
#                     table. Also deleted (rule 10): vms.ko creates OPA0: at
#                     module init and no ioctl removes a device, so a booted
#                     OVMX can never present an empty table -- MEASURED, not
#                     reasoned, in tests/qemu/test_syssvc_showdev.c and
#                     tests/uat/vms_session_qemu.sh, which both see OPA0: on
#                     every run against a real /dev/vms
#
# NOSUCHDEV is forbidden here for a different reason -- Rule 10, not row
# fabrication. The oracle measured "%SYSTEM-W-NOSUCHDEV, no such device
# available" for ONE condition: a named device the executive says does not
# exist (section 6). In this environment the executive answered nothing at
# all, so printing that message would be a false statement in VMS's own
# voice.
#
# THE POSITIVE ANCHOR IS NOW $STATUS, NOT A PRINTED MESSAGE (vms-fb9 r5).
# With no /dev/vms, vms_kif_open() fails and every subsequent ioctl this
# process issues fails too (EBADF on the resulting negative descriptor),
# which src/libvmssys/vms_kif.c's vms_kif_kerr_to_ss() maps -- through its
# closed, oracle-pinned errno set -- to SS$_BUGCHECK (676) by default. SHOW
# DEVICE no longer renders that to the user (rule 10: "the executive did not
# answer" is not a user-facing condition), but it still sets $STATUS, and
# SHOW SYMBOL $STATUS reads it back. This is what stops the test passing
# vacuously because SHOW DEVICE prints nothing whatsoever OR because DCL
# never reached it: MOUNT alone leaves $STATUS = 1 (measured, see the MOUNT-
# only case below), so 676 can ONLY appear if SHOW DEVICE itself ran the
# devscan/getdvi call and hit the no-executive ioctl failure. Measured by
# running `printf 'MOUNT DUA0: TESTDISK\nSHOW SYMBOL $STATUS\n' | DCL.EXE`
# in isolation: $STATUS = 1, not 676, confirming MOUNT's own status is not
# what this anchor is keying on.
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
# (CLAUDE.md Rule 9). It is proven in the QEMU runtime instead --
# tests/qemu/test_syssvc_showdev.c drives this same DCL binary against a real
# executive, including the A-writes/B-reads case where another process
# allocates the console and SHOW DEVICE observes it.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'MOUNT DUA0: TESTDISK\nSHOW DEVICE\nSHOW SYMBOL $STATUS\nSHOW DEVICE DUA0:\n' | $VMSDCL 2>&1
