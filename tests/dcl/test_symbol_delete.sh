#!/bin/bash
# TEST: DELETE/SYMBOL removes a symbol from the table
# EXPECT: contains:DELSYM_VALUE
# EXPECT: contains:no symbol
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'DELSYM = "DELSYM_VALUE"\nSHOW SYMBOL DELSYM\nDELETE/SYMBOL DELSYM\nSHOW SYMBOL DELSYM\n' | $VMSDCL 2>&1
