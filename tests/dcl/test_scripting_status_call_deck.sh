#!/bin/bash
# TEST: DCL .COM scripting fidelity — $STATUS refresh/representation, IF $STATUS
#       and IF .NOT. $STATUS, CALL/SUBROUTINE/RETURN [status] with a fresh local
#       scope, and DECK/EOD in-stream data delivered to SYS$INPUT (vms-3983).
#
# References (clean-room, Rule 8): VSI OpenVMS DCL Dictionary — "$STATUS",
# "$SEVERITY", "IF" (numeric expression true iff low bit set), "CALL",
# "SUBROUTINE", "RETURN", "DECK", "EOD".
#
# $STATUS after WRITE succeeds is odd → IF $STATUS is true.
# EXPECT: contains:STATUS_TRUE_ON_SUCCESS
# A mistyped command refreshes $STATUS to an (even) error → IF .NOT. $STATUS catches it.
# EXPECT: contains:STATUS_FALSE_ON_ERROR
# $STATUS renders VMS-style "%Xhhhhhhhh", not decimal.
# EXPECT: regex:\$STATUS = "%X0*1"
# CALL passes P1/P2 into a fresh subroutine level.
# EXPECT: contains:SUB_P1P2=[alpha][beta]
# RETURN [status] sets $STATUS to the returned (even) value → caller sees failure.
# EXPECT: contains:RETURN_STATUS_PROPAGATED
# A local defined in the subroutine does NOT leak back to the caller.
# EXPECT: contains:LOCAL_ISOLATED
# DECK/EOD feeds in-stream data (including a $-line) to CREATE's SYS$INPUT.
# EXPECT: contains:DOLLAR_DATA_LINE_KEPT
# The parent procedure resumes after the EOD sentinel.
# EXPECT: contains:AFTER_DECK
set -u
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

TDIR="dcl_scr_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"

cat > "/vms/$TDIR/scr.com" << 'EOF'
$ ON ERROR THEN CONTINUE
$!
$! --- $STATUS refresh + IF $STATUS truth (odd = success) ---
$ WRITE SYS$OUTPUT "hi"
$ IF $STATUS THEN WRITE SYS$OUTPUT "STATUS_TRUE_ON_SUCCESS"
$!
$! --- mistyped command refreshes $STATUS to an error; IF .NOT. catches it ---
$ NOSUCHVERB_XYZZY
$ IF .NOT. $STATUS THEN WRITE SYS$OUTPUT "STATUS_FALSE_ON_ERROR"
$!
$! --- CALL / SUBROUTINE / RETURN [status] with a fresh local scope ---
$ CALL WORKER alpha beta
$ IF .NOT. $STATUS THEN WRITE SYS$OUTPUT "RETURN_STATUS_PROPAGATED"
$ IF F$TYPE(SUBLOCAL) .EQS. "" THEN WRITE SYS$OUTPUT "LOCAL_ISOLATED"
$!
$! --- DECK / EOD in-stream data into CREATE's SYS$INPUT ---
$ CREATE DATA.TXT
$ DECK
first data line
$ this line begins with a dollar but is DATA, not a command
last data line
$ EOD
$ WRITE SYS$OUTPUT "AFTER_DECK"
$ SEARCH DATA.TXT "dollar"
$ IF $STATUS THEN WRITE SYS$OUTPUT "DOLLAR_DATA_LINE_KEPT"
$!
$! --- $STATUS renders VMS-style "%Xhhhhhhhh" (odd success shown here) ---
$ WRITE SYS$OUTPUT "ok"
$ SHOW SYMBOL $STATUS
$ EXIT
$!
$ WORKER: SUBROUTINE
$ WRITE SYS$OUTPUT "SUB_P1P2=[''P1'][''P2']"
$ SUBLOCAL = 99
$ RETURN %X10000012
$ ENDSUBROUTINE
EOF

{ echo "SET DEFAULT SYS\$SYSDEVICE:[$VDIR]"; echo "@scr.com"; } | "$VMSDCL" 2>&1

rm -rf "/vms/$TDIR"
