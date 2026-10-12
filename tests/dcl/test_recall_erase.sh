#!/bin/bash
# TEST: RECALL/ERASE empties the recall buffer (vms-7c7)
#
# After RECALL/ERASE the buffer holds no command entered before it, so a
# following RECALL/ALL lists none of them. SHOW TIME / SHOW USERS are entered
# before the erase; their literal command text must not reappear in a RECALL
# listing afterwards. (SHOW TIME prints a timestamp and SHOW USERS a user table,
# not their own command text, so the strings below can only come from a RECALL
# listing.) DCL Dictionary, RECALL/ERASE.
#
# An emptied buffer lists nothing at all -- not even the RECALL/ALL that asked:
# VMS never records a RECALL command (VAX V7.3 keystroke RC.ALL A2, rd vms-0315).
# EXPECT: contains:ERASE-LISTED
# EXPECT_NOT: regex:^ +[0-9]+ 
# EXPECT_NOT: regex:[[:space:]]+SHOW TIME
# EXPECT_NOT: regex:[[:space:]]+SHOW USERS
VMSDCL="${VMSDCL:-vmsdcl}"

printf 'SHOW TIME\nSHOW USERS\nRECALL/ERASE\nRECALL/ALL\nWRITE SYS$OUTPUT "ERASE-"+"LISTED"\n' | $VMSDCL 2>&1
