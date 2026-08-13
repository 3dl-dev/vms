#!/bin/bash
# TEST: DCL error-handling control in command procedures (vms-ada) —
#       the default "exit on error" action, SET [NO]ON, and the ON
#       WARNING|ERROR|SEVERE_ERROR THEN <command> handler, all driven by the
#       REAL $STATUS/$SEVERITY of each command (vms-3983) and scoped per command
#       level (vms-2af frames).
#
# References (clean-room, Rule 8): VSI OpenVMS DCL Dictionary — "ON",
# "SET ON"/"SET NOON"; VSI OpenVMS User's Manual — "Controlling Error
# Conditions". Severity is the low three bits of $STATUS:
# 0=WARNING, 1=SUCCESS, 2=ERROR, 3=INFO, 4=FATAL(SEVERE). By default a command
# procedure exits when a command completes with ERROR or SEVERE severity; a
# WARNING does not stop it. This suite drives two reliable severities:
#   * SEVERE  — a mistyped verb returns SS$_IVVERB (severity 4)
#   * WARNING — @ of a missing procedure returns SS$_NOSUCHFILE (severity 0)
#
# Default: SEVERE aborts the level, WARNING does not.
# EXPECT: contains:A_START
# EXPECT: contains:A_PAST_WARN
# EXPECT_NOT: contains:A_SEVERE_LEAK
# main resumes after a nested level aborted (default action returns to caller).
# EXPECT: contains:MAIN_AFTER_A
# main's SET NOON does not leak into the called level (level-scoped).
# (proven by A aborting above; main survives because of its OWN NOON.)
# A sibling level's SET NOON does not persist to a later level.
# EXPECT: contains:AA_START
# EXPECT_NOT: contains:AA_LEAK
# SET NOON continues past a SEVERE error.
# EXPECT: contains:B_NOON_CONT
# SET ON restores the default error-stop.
# EXPECT: contains:C_NOON_MID
# EXPECT_NOT: contains:C_SETON_LEAK
# ON ERROR THEN GOTO fires at the error threshold and re-arms on a new ON.
# EXPECT: contains:D_H1
# EXPECT: contains:D_H2
# EXPECT_NOT: contains:D_SKIP1
# EXPECT_NOT: contains:D_SKIP2
# The ON action is one-shot: after it fires once, an unhandled error defaults
# to abort (no infinite re-fire — the suite would time out otherwise).
# EXPECT: contains:E_ONCE
# EXPECT_NOT: contains:E_LOOP_LEAK
# ON SEVERE_ERROR ignores a WARNING (below threshold) yet fires on a SEVERE.
# EXPECT: contains:F_START
# EXPECT: contains:F_WARN_CONT
# EXPECT: contains:F_SEV_CAUGHT
# EXPECT_NOT: contains:F_SKIP
# ON WARNING catches a WARNING (lowest threshold).
# EXPECT: contains:G_WARN_CAUGHT
# EXPECT_NOT: contains:G_SKIP
# ON ERROR THEN EXIT stops the level.
# EXPECT: contains:H_START
# EXPECT_NOT: contains:H_EXIT_LEAK
# The whole procedure runs to completion.
# EXPECT: contains:MAIN_DONE
set -u
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

TDIR="dcl_errctl_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"

cat > "/vms/$TDIR/scr.com" << 'EOF'
$! Top level: SET NOON so the driver survives every sub-test and the cascade
$! stops here — each sub-test runs at its own fresh command level.
$ SET NOON
$ WRITE SYS$OUTPUT "MAIN_START"
$ CALL A
$ WRITE SYS$OUTPUT "MAIN_AFTER_A"
$ CALL B
$ CALL AA
$ CALL C
$ CALL D
$ CALL E
$ CALL F
$ CALL G
$ CALL H
$ WRITE SYS$OUTPUT "MAIN_DONE"
$ EXIT
$!
$! --- A: default action — WARNING continues, SEVERE aborts the level ---
$ A: SUBROUTINE
$   WRITE SYS$OUTPUT "A_START"
$   @NO_SUCH_PROC_AW
$   WRITE SYS$OUTPUT "A_PAST_WARN"
$   ZZBADVERB_A
$   WRITE SYS$OUTPUT "A_SEVERE_LEAK"
$ ENDSUBROUTINE
$!
$! --- AA: default action, called AFTER B's SET NOON — must still abort,
$!     proving a sibling level's NOON did not persist to this level ---
$ AA: SUBROUTINE
$   WRITE SYS$OUTPUT "AA_START"
$   ZZBADVERB_AA
$   WRITE SYS$OUTPUT "AA_LEAK"
$ ENDSUBROUTINE
$!
$! --- B: SET NOON continues past a SEVERE error ---
$ B: SUBROUTINE
$   SET NOON
$   ZZBADVERB_B
$   WRITE SYS$OUTPUT "B_NOON_CONT"
$ ENDSUBROUTINE
$!
$! --- C: SET ON restores the default error-stop ---
$ C: SUBROUTINE
$   SET NOON
$   ZZBADVERB_C1
$   WRITE SYS$OUTPUT "C_NOON_MID"
$   SET ON
$   ZZBADVERB_C2
$   WRITE SYS$OUTPUT "C_SETON_LEAK"
$ ENDSUBROUTINE
$!
$! --- D: ON ERROR THEN GOTO fires on SEVERE, and re-arms on a fresh ON ---
$ D: SUBROUTINE
$   ON ERROR THEN GOTO DH1
$   ZZBADVERB_D1
$   WRITE SYS$OUTPUT "D_SKIP1"
$ DH1:
$   WRITE SYS$OUTPUT "D_H1"
$   ON ERROR THEN GOTO DH2
$   ZZBADVERB_D2
$   WRITE SYS$OUTPUT "D_SKIP2"
$ DH2:
$   WRITE SYS$OUTPUT "D_H2"
$ ENDSUBROUTINE
$!
$! --- E: the ON action is one-shot; a second unhandled error defaults to
$!     abort rather than re-firing (else this loops and the suite times out) ---
$ E: SUBROUTINE
$   ON ERROR THEN GOTO EH
$   ZZBADVERB_E1
$   WRITE SYS$OUTPUT "E_SKIP"
$ EH:
$   WRITE SYS$OUTPUT "E_ONCE"
$   ZZBADVERB_E2
$   WRITE SYS$OUTPUT "E_LOOP_LEAK"
$ ENDSUBROUTINE
$!
$! --- F: ON SEVERE_ERROR ignores a WARNING (below threshold) yet fires on
$!     a SEVERE (at threshold) ---
$ F: SUBROUTINE
$   ON SEVERE_ERROR THEN GOTO FH
$   WRITE SYS$OUTPUT "F_START"
$   @NO_SUCH_PROC_F
$   WRITE SYS$OUTPUT "F_WARN_CONT"
$   ZZBADVERB_F
$   WRITE SYS$OUTPUT "F_SKIP"
$ FH:
$   WRITE SYS$OUTPUT "F_SEV_CAUGHT"
$ ENDSUBROUTINE
$!
$! --- G: ON WARNING catches a WARNING (lowest threshold) ---
$ G: SUBROUTINE
$   ON WARNING THEN GOTO GH
$   @NO_SUCH_PROC_G
$   WRITE SYS$OUTPUT "G_SKIP"
$ GH:
$   WRITE SYS$OUTPUT "G_WARN_CAUGHT"
$ ENDSUBROUTINE
$!
$! --- H: ON ERROR THEN EXIT stops the level ---
$ H: SUBROUTINE
$   ON ERROR THEN EXIT
$   WRITE SYS$OUTPUT "H_START"
$   ZZBADVERB_H
$   WRITE SYS$OUTPUT "H_EXIT_LEAK"
$ ENDSUBROUTINE
EOF

{ echo "SET DEFAULT SYS\$SYSDEVICE:[$VDIR]"; echo "@scr.com"; } | "$VMSDCL" 2>&1

rm -rf "/vms/$TDIR"
