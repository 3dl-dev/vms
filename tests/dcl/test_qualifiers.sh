#!/bin/bash
# TEST: /LOG and /CONFIRM qualifiers work on file commands
# EXPECT: contains:COPIED
# EXPECT: contains:FILDEL
# EXPECT: regex:[0-9]+:.*hello
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_qual_test_$$"
mkdir -p "/vms/$TDIR"
echo "test" > "/vms/$TDIR/QUAL1.TXT"
echo "hello world" > "/vms/$TDIR/QUAL2.TXT"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
# DELETE needs an explicit version in VMS (vms-1c6); ;* selects all versions.
# DELETE /LOG reports the authentic %DELETE-I-FILDEL identifier.
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nCOPY QUAL1.TXT QUAL1B.TXT /LOG\nDELETE QUAL1B.TXT;* /LOG\nSEARCH QUAL2.TXT "hello" /NUMBERS\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR"
