#!/bin/bash
# TEST: SHOW TIME with no executive reports SS$_NOSUCHDEV, never the host clock
# The system time is the executive's (rd vms-642): SHOW TIME is SYS$GETTIM, and
# this host suite runs DCL with no /dev/vms. The honest answer is the condition,
# not a time read from the substrate. The display itself -- "  29-AUG-2026
# 00:56:01", two leading blanks, no hundredths, $STATUS %X00030001 as on VAX
# V7.3 and Alpha V8.4 (tests/qemu/golden/oracle-*.console.txt,
# docs/oracle/vax73-status-after-verbs.md) -- is asserted against a real
# executive by tests/qemu/test_syssvc_systime.c.
# EXPECT: contains:%SYSTEM-W-NOSUCHDEV, no such device available
# EXPECT_NOT: regex:[0-9]{1,2}-[A-Z]{3}-[0-9]{4} [0-9]{2}:[0-9]{2}
# EXPECT: contains:$STATUS == "%X00000908"
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SHOW TIME\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
