#!/bin/bash
# TEST: WRITE SYS$OUTPUT with string, symbol, and comma concatenation
# EXPECT: contains:WRITE_LITERAL_OK
# EXPECT: contains:WRITE_SYMBOL_OK
# EXPECT: contains:PartAPartB
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'WRITE SYS$OUTPUT "WRITE_LITERAL_OK"\nX = "WRITE_SYMBOL_OK"\nWRITE SYS$OUTPUT '"'"'X'"'"'\nWRITE SYS$OUTPUT "PartA","PartB"\n' | $VMSDCL 2>&1
