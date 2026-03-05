#!/bin/bash
# TEST: Local symbol assignment and substitution in WRITE
# EXPECT: contains:hello_local
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'X = "hello_local"\nWRITE SYS$OUTPUT '"'"'X'"'"'\n' | $VMSDCL 2>&1
