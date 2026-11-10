#!/bin/bash
# TEST: Script GOTO skips to label, code before label not executed
# EXPECT: contains:AFTER_LABEL
# EXPECT_NOT: contains:BEFORE_LABEL
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
# Staged on the system disk: @ reaches a procedure only through RMS
# $OPEN/$GET, never a host path (libc-drop vms-20c94).
TNAME="dcl_t_$$"
TDIR="/vms/$TNAME"
VDIR="$(echo "$TNAME" | tr a-z A-Z)"
mkdir -p "$TDIR"
printf '$ GOTO SKIP\n$ WRITE SYS$OUTPUT "BEFORE_LABEL"\n$ SKIP:\n$ WRITE SYS$OUTPUT "AFTER_LABEL"\n$ EXIT\n' > "$TDIR/labels.com"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\n@labels.com\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "$TDIR"
