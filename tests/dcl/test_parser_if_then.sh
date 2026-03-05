#!/bin/bash
# TEST: IF/THEN/ELSE control flow with string and numeric comparisons
# EXPECT: contains:NUM_TRUE
# EXPECT: contains:STR_TRUE
# EXPECT_NOT: contains:NUM_FALSE
# EXPECT_NOT: contains:STR_FALSE
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'IF 1 .EQ. 1 THEN WRITE SYS$OUTPUT "NUM_TRUE"\nIF 1 .EQ. 2 THEN WRITE SYS$OUTPUT "NUM_FALSE"\nIF "ABC" .EQS. "ABC" THEN WRITE SYS$OUTPUT "STR_TRUE"\nIF "ABC" .EQS. "DEF" THEN WRITE SYS$OUTPUT "STR_FALSE"\n' | $VMSDCL 2>&1
