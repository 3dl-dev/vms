#!/bin/bash
# TEST: RECALL replays commands from DCL's own recall buffer (readline-independent)
#
# vms-7c7: RECALL is a property of the command interpreter, not of any terminal
# line-editing package. It must work in every build -- including the static musl
# runtime that has no readline -- driven from DCL's own recall buffer, never
# reporting "requires readline support" (that facade was the INV-DCL tell). The
# commands below are fed on a non-tty pipe (readline inactive) and RECALL must
# still see them. As the VAX V7.3 console shows it (keystroke RC.ALL, rd
# vms-0315): RECALL/ALL numbers from the most recent command (1), "%3d %s",
# and never lists a RECALL command; RECALL n brings the command back to the
# next command line for editing -- it does not run it.
#
# EXPECT: regex:^  1 SHOW DEFAULT$
# EXPECT: regex:^  2 SHOW TIME$
# EXPECT: contains:%DCL-W-CMDNOTFND, command not found - use RECALL/ALL to display saved commands
# EXPECT: contains:RECALL-COUNT-1
# EXPECT_NOT: contains:RECALL-COUNT-2
# EXPECT_NOT: regex:^ +[0-9]+ RECALL
# EXPECT_NOT: contains:requires readline support
VMSDCL="${VMSDCL:-vmsdcl}"

echo "--- RECALL/ALL numbered list ---"
printf 'SHOW TIME\nSHOW DEFAULT\nRECALL/ALL\n' | $VMSDCL 2>&1

echo "--- RECALL n does not execute command number n ---"
printf '$ N = 0\n$ N = N + 1\nRECALL 1\nWRITE SYS$OUTPUT "RECALL-COUNT-", N\n' | $VMSDCL 2>&1

echo "--- RECALL of an out-of-range number ---"
printf 'SHOW TIME\nRECALL 9\n' | $VMSDCL 2>&1
