#!/bin/bash
# TEST: Qualifier parsing with /QUALIFIER syntax
#
# SHOW PROCESS/ALL exercises the parser's /QUALIFIER handling: /ALL must be
# ACCEPTED as a qualifier (not rejected as %DCL-W-IVQUAL) and reach the
# command handler. This test used to assert the handler's output contained
# "Process Name" and "PID" -- the headings of a fabricated one-row process
# table. vms-70eb deleted that table (docs/oracle/vax73-show-system-process.md
# Section 4: VMS /ALL is "all info about THIS process", not a process table),
# so /ALL now flows through the shared $GETJPI display path and, on a host
# with no executive, degrades to the same %SYSTEM condition plain SHOW PROCESS
# reports. That the command RAN (a %SYSTEM condition, not a parser IVQUAL) is
# the parser proof this test exists for; the absence of the old table headings
# keeps it from re-endorsing the fabrication.
#
# EXPECT: regex:%SYSTEM-[A-Z]-
# EXPECT_NOT: regex:%DCL-W-IVQUAL
# EXPECT_NOT: contains:Processes at
# EXPECT_NOT: contains:LEF
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SHOW PROCESS/ALL\n' | $VMSDCL 2>&1
