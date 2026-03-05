#!/bin/bash
# TEST: F$SEARCH finds files
# EXPECT: contains:DCL_FSEARCH_TEST
# EXPECT_NOT: contains:%DCL-
VMSDCL="${VMSDCL:-vmsdcl}"
TMPFILE="/tmp/DCL_FSEARCH_TEST_$$.tmp"
touch "$TMPFILE"
printf 'X = F$SEARCH("'"$TMPFILE"'")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
rm -f "$TMPFILE"
