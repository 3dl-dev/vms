#!/bin/bash
# TEST: SHOW PROCESS/IDENTIFICATION rejects a malformed value the way VMS does
#
# ORACLE-PINNED (vms-6a7), docs/oracle/vax73-show-system-process.md Section
# 3.2.2. On VAX1, OpenVMS VAX V7.3:
#
#   $ SHOW PROCESS/ID=ZZZZ
#   %SHOW-E-INVQUAVAL, value 'ZZZZ' invalid for /IDENTIFICATION qualifier
#
# Facility SHOW (the command, not SYSTEM), severity E, and the offending value
# single-quoted. This is a DCL-layer rejection that never reaches $GETJPI --
# which is precisely why it is testable on a host with no /dev/vms, and why it
# must NOT be reported as NONEXPR: "ZZZZ" is not a process that does not exist,
# it is not a process ID at all.
#
# This test also proves the /ID abbreviation resolves. dcl_qualifier_value()
# matches qualifier names EXACTLY, so /ID is invisible to it; the command uses
# the parser's minimum-uniqueness matcher instead. If that regressed, /ID would
# be ignored, no value would be parsed, SHOW PROCESS would fall through to the
# self case, and this message would never be printed.
#
# EXPECT: regex:^%SHOW-E-INVQUAVAL, value 'ZZZZ' invalid for /IDENTIFICATION qualifier$
# EXPECT_NOT: contains:NONEXPR
# EXPECT_NOT: contains:bash
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW PROCESS/ID=ZZZZ" | $VMSDCL 2>&1
