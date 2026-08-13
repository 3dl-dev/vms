#!/bin/bash
# TEST: RECALL replays commands from DCL's own recall buffer (readline-independent)
#
# vms-7c7: RECALL is a property of the command interpreter, not of any terminal
# line-editing package. It must work in every build -- including the static musl
# runtime that has no readline -- driven from DCL's own recall buffer, never
# reporting "requires readline support" (that facade was the INV-DCL tell). The
# commands below are fed on a non-tty pipe (readline inactive) and RECALL must
# still see them. DCL Dictionary, RECALL: the buffer holds the recent commands,
# RECALL/ALL numbers them 1..N (oldest..newest), RECALL n replays number n.
#
# EXPECT: regex:1[[:space:]]+SHOW TIME
# EXPECT: regex:2[[:space:]]+SHOW DEFAULT
# EXPECT: contains:RECALL-N-REEXEC-OK
# EXPECT: contains:%DCL-W-RECALL, no command number 9 in history
# EXPECT_NOT: contains:requires readline support
VMSDCL="${VMSDCL:-vmsdcl}"

echo "--- RECALL/ALL numbered list ---"
printf 'SHOW TIME\nSHOW DEFAULT\nRECALL/ALL\n' | $VMSDCL 2>&1

echo "--- RECALL n re-executes command number n ---"
printf 'WRITE SYS$OUTPUT "RECALL-N-REEXEC-OK"\nRECALL 1\n' | $VMSDCL 2>&1

echo "--- RECALL of an out-of-range number ---"
printf 'SHOW TIME\nRECALL 9\n' | $VMSDCL 2>&1
