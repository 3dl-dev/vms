#!/bin/bash
# TEST: SHOW TIME displays VMS date format (DD-MMM-YYYY HH:MM:SS)
# EXPECT: regex:[0-9]{1,2}-[A-Z]{3}-[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}
# EXPECT_NOT: contains:/
# No hundredths: VAX V7.3 and Alpha V8.4 print "  29-AUG-2026 00:56:01"
# (tests/qemu/golden/oracle-*.console.txt).
# EXPECT_NOT: regex:[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]
# EXPECT: regex:^  [ 1-3][0-9]-[A-Z]{3}-[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}$
# $STATUS after SHOW TIME is DCL's own success (docs/oracle/vax73-status-after-verbs.md)
# EXPECT: contains:$STATUS == "%X00030001"
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SHOW TIME\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
