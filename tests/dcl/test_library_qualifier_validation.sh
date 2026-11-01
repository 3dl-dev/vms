#!/bin/bash
# TEST: LIBRARY refuses a qualifier it does not implement before any library is touched (vms-097).
# EXPECT: contains:%DCL-W-IVQUAL
# EXPECT: contains: \ZQONE\
# EXPECT: contains: \MACRO\
# EXPECT: contains: \VAX\
# EXPECT: contains:LIBRARIAN-E-OPENIN
# EXPECT_NOT: contains: \COMPRESS\
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'LIBRARY/ZQONE NOSUCH.TLB\nLIBRARY/MACRO NOSUCH.TLB\nLIBRARY/OBJECT/COMPRESS=BLOCK=0 NOSUCH.OLB\nLIBRARY/OBJECT/VAX NOSUCH.OLB\n' | $VMSDCL 2>&1
