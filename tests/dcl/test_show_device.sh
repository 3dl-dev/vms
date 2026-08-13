#!/bin/bash
# TEST: SHOW DEVICE prints no device row it did not read from the executive
# EXPECT: contains:DCL-ALIVE
# EXPECT: contains:$STATUS = "%X000002A4"
# EXPECT_NOT: regex:^Device +Device +Error
# EXPECT_NOT: contains:NOSUCHDEV
# EXPECT_NOT: regex:^\$1\$DGA
# EXPECT_NOT: contains:OVMXSYS
# EXPECT_NOT: contains:Error    Volume
# EXPECT_NOT: regex:^[A-Z0-9$_]+: +(Mounted|Dismounted)
# EXPECT_NOT: contains:%OVMX-F-EXECDEV
# EXPECT_NOT: contains:%OVMX-F-NODEVTAB
# EXPECT_NOT: contains:%MOUNT-I-MOUNTED
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
#   %MOUNT-I-MOUNTED  MOUNT's own facade success message, deleted product-wide
#                     by vms-651 (real mount(2), never a per-process fake) --
#                     kept here as history the same way the others are.
#
# NOSUCHDEV is forbidden here for a different reason -- Rule 10, not row
# fabrication. The oracle measured "%SYSTEM-W-NOSUCHDEV, no such device
# available" for ONE condition: a named device the executive says does not
# exist (section 6). In this environment the executive answered nothing at
# all, so printing that message would be a false statement in VMS's own
# voice.
#
# THE POSITIVE ANCHOR IS $STATUS, NOT A PRINTED MESSAGE (vms-fb9 r5). With no
# /dev/vms, vms_kif_open() fails and every subsequent ioctl this process
# issues fails too (EBADF on the resulting negative descriptor), which
# src/libvmssys/vms_kif.c's vms_kif_kerr_to_ss() maps -- through its closed,
# oracle-pinned errno set -- to SS$_BUGCHECK (676) by default. SHOW DEVICE no
# longer renders that to the user (rule 10: "the executive did not answer" is
# not a user-facing condition), but it still sets $STATUS, and SHOW SYMBOL
# $STATUS reads it back.
#
# THE LIVENESS ANCHOR CHANGED FROM MOUNT TO A BARE WRITE (vms-651). Before
# vms-651, MOUNT DUA0: TESTDISK succeeded even with no executive (the
# facade), leaving $STATUS = 1 -- proof DCL was alive and running commands
# BEFORE SHOW DEVICE, so a later $STATUS = 676 could only have come from
# SHOW DEVICE itself. vms-651 deleted that facade: MOUNT now asks the
# executive too (vms_kif_chkpriv before anything else), so with no /dev/vms
# it ALSO fails via the same ioctl path and ALSO leaves $STATUS = 676 --
# using it as the liveness anchor would make this test pass vacuously
# whether or not SHOW DEVICE itself touched the executive. `WRITE SYS$OUTPUT`
# never asks the executive at all, so it is now the anchor: EXPECT
# "DCL-ALIVE" proves the session ran a command, and it leaves $STATUS = 1,
# so a later $STATUS = 676 can only be SHOW DEVICE's.
#
# ANCHORING (this repo has shipped assertions satisfiable by something other
# than the behaviour under test, so it is spelled out): the EXPECT_NOT
# patterns are line-anchored or contain column runs precisely because the
# old fabricated rows started at column 0 with the device name.
#
# WHAT IS NOT ASSERTED HERE: that SHOW DEVICE shows OPA0: when an executive
# IS present. That needs a real /dev/vms and cannot run under ctest at all
# (CLAUDE.md Rule 9). It is proven in the QEMU runtime instead --
# tests/qemu/test_syssvc_showdev.c drives this same DCL binary against a real
# executive, including the A-writes/B-reads case where another process
# allocates the console and SHOW DEVICE observes it.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'WRITE SYS$OUTPUT "DCL-ALIVE"\nSHOW DEVICE\nSHOW SYMBOL $STATUS\nSHOW DEVICE DUA0:\n' | $VMSDCL 2>&1
