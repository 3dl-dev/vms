#!/bin/bash
# TEST: $STATUS symbol reflects last command status, in the VMS "%Xhhhhhhhh"
#       representation (vms-3983 — SHOW SYMBOL $STATUS on real VMS shows exactly
#       "%X00000001" for SS$_NORMAL, NOT decimal "1").
# EXPECT: regex:\$STATUS = "%X0*1"
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'WRITE SYS$OUTPUT "ok"\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
