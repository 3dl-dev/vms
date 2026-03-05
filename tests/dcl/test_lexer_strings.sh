#!/bin/bash
# TEST: DCL string literal parsing including embedded quotes
# EXPECT: contains:Hello World
# EXPECT: contains:He said "hello"
# EXPECT: contains:EMPTY_OK
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'WRITE SYS$OUTPUT "Hello World"\nWRITE SYS$OUTPUT "He said ""hello"""\nWRITE SYS$OUTPUT ""\nWRITE SYS$OUTPUT "EMPTY_OK"\n' | $VMSDCL 2>&1
