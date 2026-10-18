#!/bin/bash
# TEST: DUMP shows a file's bytes (vms-5303e: through the ACP; the host defer reads them off the volume dir)
# EXPECT: contains:48454C4C 4F5F564D 535F4455
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_test_$$"
mkdir -p "/vms/$TDIR"
printf 'HELLO_VMS_DUMP_TEST' > "/vms/$TDIR/dumpfile.dat"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nDUMP dumpfile.dat\n' "$(echo "$TDIR" | tr a-z A-Z)" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR"
