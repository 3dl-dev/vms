#!/bin/bash
# TEST: Symbol assignment and SHOW SYMBOL work
# EXPECT: contains:HELLO_VMS
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'MYSYM = "HELLO_VMS"\nSHOW SYMBOL MYSYM\n' | $VMSDCL 2>&1
