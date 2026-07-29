#!/bin/bash
# TEST: SHOW quick-win subcommands produce correct VMS-format output
# EXPECT: contains:OVMX
# EXPECT: contains:OVMX-TCP/IP
# EXPECT: contains:%SYSTEM-I-NOTMEMBER
# EXPECT: regex:OVMX TCP/IP Services V[0-9]+\.[0-9]+
# EXPECT: contains:Node: OVMX
# EXPECT: contains:Device Error Count Summary
# EXPECT: contains:No errors logged.
# EXPECT: contains:Working Set
# EXPECT: contains:Accounting is currently disabled.
# EXPECT: contains:SYS$MANAGER:ACCOUNTNG.DAT
# EXPECT: contains:security auditing is currently disabled.
# EXPECT: contains:SYS$MANAGER:AUDIT.LOG
# EXPECT: contains:User [200,1]
# EXPECT: contains:System root is SYS$SYSDEVICE:[SYS0.SYSCOMMON.]
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'SHOW LICENSE\nSHOW CLUSTER\nSHOW NETWORK\nSHOW ERROR\nSHOW WORKING_SET\nSHOW ACCOUNTING\nSHOW AUDIT\nSHOW QUOTA\nSHOW ROOT\n' | $VMSDCL 2>&1
