#!/bin/bash
# TEST: Command procedures (@file) execute DCL scripts
# EXPECT: contains:PROCEDURE_OUTPUT_OK
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_test_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"
cat > "/vms/$TDIR/testproc.com" << 'EOF'
$ WRITE SYS$OUTPUT "PROCEDURE_OUTPUT_OK"
$ EXIT
EOF
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\n@testproc.com\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR"
