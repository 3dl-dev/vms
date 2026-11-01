#!/bin/bash
# TEST: RUN refuses an unknown or ambiguous qualifier before it is entered (vms-097).
# EXPECT: contains:%DCL-W-IVQUAL
# EXPECT: contains: \ZQBOGUS\
# EXPECT: contains:%DCL-W-ABKEYW
# EXPECT_NOT: contains: \DETACH\
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'RUN/ZQBOGUS NOSUCH_IMAGE\nRUN/P=4 NOSUCH_IMAGE\nRUN/DETACH NOSUCH_IMAGE\n' | $VMSDCL 2>&1
