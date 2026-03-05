#!/bin/bash
# TEST: DCL exits cleanly on EOF after executing commands
# EXPECT: regex:[0-9]{1,2}-[A-Z]{3}-[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}
VMSDCL="${VMSDCL:-vmsdcl}"
# Send SHOW TIME then EOF — in interactive mode Ctrl+Z (VEOF=0x1A) triggers
# this same path; piped input uses regular EOF.
printf 'SHOW TIME\n' | $VMSDCL 2>&1
