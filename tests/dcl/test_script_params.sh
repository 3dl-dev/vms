#!/bin/bash
# TEST: Script parameter substitution with P1 and P2
# EXPECT: contains:P1=ALPHA
# EXPECT: contains:P2=BETA
# EXPECT_NOT: contains:UNDSYM
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
# Staged on the system disk: @ reaches a procedure only through RMS
# $OPEN/$GET, never a host path (libc-drop vms-20c94).
TNAME="dcl_t_$$"
TDIR="/vms/$TNAME"
VDIR="$(echo "$TNAME" | tr a-z A-Z)"
mkdir -p "$TDIR"
# WRITE takes an expression: the parameters go in as a quoted string
# ("''P1'"); a bare 'P1' would make ALPHA an undefined symbol, as on VMS.
printf '$ WRITE SYS$OUTPUT "P1=%s"\n$ WRITE SYS$OUTPUT "P2=%s"\n$ EXIT\n' "''P1'" "''P2'" > "$TDIR/params.com"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\n@params.com ALPHA BETA\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "$TDIR"
