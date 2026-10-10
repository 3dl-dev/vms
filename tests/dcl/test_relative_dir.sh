#!/bin/bash
# TEST: a relative directory ([.SUB], [-]) is taken from the default directory
#
# rd vms-457: under SYS$LOGIN, CREATE/DIRECTORY [.KSPGD] and OPEN/WRITE
# [.KSPGD]F1.TXT name a subdirectory of the default directory, as on VMS (VAX
# V7.3 keystroke setup A2.SETUP3). They used to resolve to the top of the
# device ("SYS$SYSROOT:[.KSPGD]") and fail.
#
# EXPECT: contains:RELDIR-WROTE
# EXPECT: regex:Directory .*\[RELDIRT_[0-9]+\.SUB\]
# EXPECT: contains:F1.TXT;1
# EXPECT: regex:^  SYS\$SYSDEVICE:\[RELDIRT_[0-9]+\]$
# EXPECT: regex:^  SYS\$SYSDEVICE:\[RELDIRT_[0-9]+\.SUB\]$
# EXPECT_NOT: contains:%RMS-E-DNF
VMSDCL="${VMSDCL:-vmsdcl}"
VDIR="RELDIRT_$$"
mkdir -p "/vms/$VDIR/SUB"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nOPEN/WRITE F [.SUB]F1.TXT\nWRITE F "X"\nCLOSE F\nWRITE SYS$OUTPUT "RELDIR-WROTE"\nDIRECTORY [.SUB]\nSET DEFAULT [.SUB]\nSHOW DEFAULT\nSET DEFAULT [-]\nSHOW DEFAULT\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$VDIR"
