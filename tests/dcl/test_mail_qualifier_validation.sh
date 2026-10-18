#!/bin/bash
# TEST: MAIL refuses an unrecognized qualifier before MAIL.EXE is entered (vms-097).
# EXPECT: contains:%DCL-W-IVQUAL
# EXPECT: contains: \ZQBOGUS\
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'MAIL/ZQBOGUS NOSUCHUSR\n' | $VMSDCL 2>&1
