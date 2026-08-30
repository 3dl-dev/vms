#!/bin/bash
# TEST: New lexical functions F$PID, F$DEVICE, F$GETDVI, F$IDENTIFIER, F$CVUI work
# EXPECT: regex:P =
# EXPECT: regex:D =
# EXPECT: regex:C = 65
# EXPECT_NOT: contains:_OPA0:
# EXPECT_NOT: contains:_FTA0:
# EXPECT_NOT: contains:_SYS$SYSDEVICE:
# EXPECT_NOT: contains:I = "[
#
# A SECOND ASSERTION CHANGED, SAME REASON AS THE FIRST (vms-2f8). This file
# used to carry
#   # EXPECT: contains:SYSTEM
# for the F$IDENTIFIER(65540,"NUMBER_TO_NAME") line. That passed because
# lex_identifier() held a hardcoded [1,4] -> "SYSTEM" case: the assertion was
# satisfied by a literal in the function, not by a rights database.
#
# F$IDENTIFIER now READS the rights database (SYS$SYSTEM:RIGHTSLIST.DAT for
# general identifiers, SYSUAF for UIC identifiers -- src/libvms/rtl/
# rightslist.c), so under ctest, where nothing has provisioned a system disk,
# it answers the MISS. That is the correct behaviour and it is pinned to the
# oracle: an identifier that is not valid converts to the null string in this
# direction. A missing facility must answer the miss, not resurrect a
# built-in table (CLAUDE.md Rule 9).
#
# The claim did not disappear -- it MOVED SOMEWHERE STRONGER. Whether
# F$IDENTIFIER resolves SYSTEM, DEFAULT, GUEST and the six environmental
# identifiers, in BOTH directions, against the actual shipped data files, is
# now tests/libvms/test_rightslist.c: 34 assertions, every value measured on
# OpenVMS VAX V7.3 (docs/oracle/vax73-rights-database.md), staged under a
# private root so it does not depend on the host's /vms at all. One
# `contains:SYSTEM` that a hardcode satisfied is not coverage that file
# lacks.
#
# What replaces it HERE is an assertion that is true in both environments and
# was not true before: `I` must never come back as a bracketed UIC. That
# rendering -- echoing the caller's own UIC back as "[1,4]" -- was Rule 10's
# illegal third answer, refuted against the oracle for every input shape
# tried, and it is the one wrong answer this line can still produce whether
# or not a rights database is present.
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
# THE F$GETDVI ASSERTION CHANGED, SAME REASON AS F$DEVICE (vms-050). This file
# used to carry
#   # EXPECT: contains:SYS$SYSDEVICE
# for the F$GETDVI("SYS$SYSDEVICE","DEVNAM") line, and the note here reported
# that F$GETDVI was "still a fabricator ... not fixed here": lex_getdvi()
# answered EXISTS=TRUE for every name, guessed DEVCLASS/DEVTYPE from a
# name substring, returned VOLNAM="OVMXSYS"/MOUNTCNT="1" and statvfs("/")
# block counts, and rendered DEVNAM as "_<whatever-you-typed>:" -- which is
# why "SYS$SYSDEVICE" appeared whether or not any such device existed.
#
# F$GETDVI now reads the executive's device table (vms_kif_getdvi_devnam, the
# same reader F$DEVICE and SHOW DEVICE use) and the ACP mounted-volume table
# (vms_kif_getvol), so it returns the executive's own physical name (e.g.
# "VDA0:") and real volume state -- and, exactly like F$DEVICE, NOTHING where
# there is no executive to ask, as under ctest (it emits %SYSTEM-W-NOSUCHDEV
# and leaves G empty). The assertion is inverted rather than dropped: the old
# fabricated "_SYS$SYSDEVICE:" DEVNAM rendering must NOT appear, which stays
# true in BOTH environments (a real executive returns the resolved physical
# unit name, never the logical spelled back with an underscore). The POSITIVE
# proof that F$GETDVI reads real data -- EXISTS=FALSE for a bogus device, real
# VOLNAM/DEVTYPE for a real one -- runs against a real /dev/vms in
# tests/dcl/test_getdvi_no_fabrication.sh (vms-050), which a userspace-only
# ctest cannot stand in for (CLAUDE.md Rule 9).
#
# The other three lexicals are untouched and their assertions are unchanged;
# they are what keeps this from becoming a test that passes because DCL
# printed nothing.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'P = F$PID("")\nSHOW SYMBOL P\nD = F$DEVICE("*")\nSHOW SYMBOL D\nG = F$GETDVI("SYS$SYSDEVICE","DEVNAM")\nSHOW SYMBOL G\nI = F$IDENTIFIER(65540,"NUMBER_TO_NAME")\nSHOW SYMBOL I\nC = F$CVUI(0,8,"A")\nSHOW SYMBOL C\n' | $VMSDCL 2>&1
