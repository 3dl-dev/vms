#!/bin/bash
# TEST: Symbol substitution with apostrophe syntax
# EXPECT: contains:SUBSTITUTED_VALUE
VMSDCL="${VMSDCL:-vmsdcl}"
printf "TESTVAR = \"SUBSTITUTED_VALUE\"\nWRITE SYS\$OUTPUT 'TESTVAR'\n" | $VMSDCL 2>&1
