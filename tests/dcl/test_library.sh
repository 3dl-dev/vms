#!/bin/bash
# TEST: LIBRARY command creates, inserts, lists, and extracts modules
# EXPECT: contains:LIBRARIAN-S-CREATED
# EXPECT: contains:LIBRARIAN-S-INSERTED
# EXPECT: contains:TESTMOD
# EXPECT: contains:Hello from test module
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_test_lib_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"

# Create a source text file
printf "Hello from test module\nLine two\n" > "/vms/$TDIR/source.txt"

# Run DCL commands: create library, insert module, list, extract
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]
LIBRARY /CREATE test.TLB
LIBRARY test.TLB TESTMOD source.txt
LIBRARY /LIST test.TLB
LIBRARY /EXTRACT=TESTMOD test.TLB
' "$VDIR" | $VMSDCL 2>&1

rm -rf "/vms/$TDIR"
