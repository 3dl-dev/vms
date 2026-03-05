#!/bin/bash
# TEST: WRITE SYS$OUTPUT sends text to stdout
# EXPECT: contains:VMS_WRITE_TEST_OK
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'WRITE SYS$OUTPUT "VMS_WRITE_TEST_OK"\n' | $VMSDCL 2>&1
