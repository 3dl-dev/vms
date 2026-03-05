#!/bin/bash
# TEST: Global symbol assignment with == and SHOW SYMBOL
# EXPECT: contains:global_test_value
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'GSYM == "global_test_value"\nSHOW SYMBOL GSYM\n' | $VMSDCL 2>&1
