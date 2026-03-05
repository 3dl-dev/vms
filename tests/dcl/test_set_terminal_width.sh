#!/bin/bash
# TEST: SET TERMINAL /WIDTH=132 updates width in SHOW TERMINAL output
# EXPECT: contains:Width: 132
VMSDCL="${VMSDCL:-vmsdcl}"
printf "SET TERMINAL /WIDTH=132\nSHOW TERMINAL\n" | $VMSDCL 2>&1
