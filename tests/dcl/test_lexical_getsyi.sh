#!/bin/bash
# TEST: F$GETSYI returns system information
# The VMS-compat version is TRUE-TO-ARCH (INV-1): V9.2-x on x86-64, OVMX's
# own version where the arch has no VMS lineage. Assert a well-formed token
# rather than a literal, and assert the pre-INV-1 hardcode is gone.
# EXPECT: regex:X = "V[0-9]+\.[0-9]+(-[0-9]+)?"
# EXPECT_NOT: contains:V7.3
# EXPECT_NOT: contains:%DCL-
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'X = F$GETSYI("VERSION")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
