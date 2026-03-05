#!/bin/bash
# TEST: SET VERIFY and SET NOVERIFY change verification state
# EXPECT: contains:VERIFY = ON
# EXPECT: contains:VERIFY = OFF
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'SET VERIFY\nSHOW VERIFY\nSET NOVERIFY\nSHOW VERIFY\n' | $VMSDCL 2>&1
