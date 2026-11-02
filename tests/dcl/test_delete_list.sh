#!/bin/bash
# TEST: DELETE deletes every file of a comma-separated list, not only the first
# EXPECT: contains:DELLIST A= B= C=KEEP
#
# VMS DELETE takes a list (DELETE A.TMP;*,B.TMP;*); the parser hands DCL each
# element as its own parameter, and DELETE used to act on the first one only
# (rd vms-ad5a). C.TMP is not in the list and must survive.
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_dellist_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"
cat <<EOD | "$VMSDCL" 2>&1
SET DEFAULT SYS\$SYSDEVICE:[$VDIR]
OPEN/WRITE F A.TMP
CLOSE F
OPEN/WRITE F B.TMP
CLOSE F
OPEN/WRITE F B.TMP
CLOSE F
OPEN/WRITE F C.TMP
CLOSE F
DELETE A.TMP;*,B.TMP;*
SP_C = "KEEP"
IF F\$SEARCH("C.TMP;*") .EQS. "" THEN SP_C = "GONE"
WRITE SYS\$OUTPUT "DELLIST A=", F\$SEARCH("A.TMP;*"), " B=", F\$SEARCH("B.TMP;*"), " C=", SP_C
EOD
rm -rf "/vms/$TDIR"
