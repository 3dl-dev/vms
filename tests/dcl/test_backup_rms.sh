#!/bin/bash
# TEST: BACKUP reaches savesets through RMS: a missing saveset is OPENIN with the RMS reason, and a saveset name takes the .BCK default type
# EXPECT: contains:%BACKUP-E-OPENIN, error opening NOSUCH_SAVESET as input
# EXPECT: contains:-RMS-E-FNF, file not found
# EXPECT: contains:%BACKUP-S-SAVESET, saveset SAVED created, 1 file
# EXPECT: contains:Listing of save set SAVED
# EXPECT: contains:RMSBCK.TXT
# EXPECT: contains:Total of 1 file
# EXPECT_NOT: contains:No such file
#
# libc-drop vms-539: BACKUP opens its saveset with RMS ($OPEN, block $READ),
# so a saveset RMS cannot find is reported as VMS does -- the OPENIN message
# and the RMS reason on its own line, never a host strerror() text -- and a
# saveset named without a file type is the .BCK default type RMS supplies.
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_backup_rms_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$VDIR"
echo "RMS_BACKUP_DATA" > "/vms/$VDIR/rmsbck.txt"

printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nBACKUP /LIST NOSUCH_SAVESET\n' "$VDIR" | $VMSDCL 2>&1
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nBACKUP RMSBCK.TXT SAVED /SAVE_SET\nBACKUP /LIST SAVED\n' "$VDIR" | $VMSDCL 2>&1

rm -rf "/vms/$VDIR"
