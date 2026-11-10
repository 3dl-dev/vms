#!/bin/bash
# TEST: DCL exits cleanly on EOF after executing commands
# EXPECT: contains:EOF-PROBE-DONE
VMSDCL="${VMSDCL:-vmsdcl}"
# Send one command then EOF -- in interactive mode Ctrl+Z (VEOF=0x1A) triggers
# this same path; piped input uses regular EOF. The command is a WRITE, which
# needs no executive (SHOW TIME reads the executive's system time, rd vms-642,
# and this host suite has none).
printf 'WRITE SYS$OUTPUT "EOF-PROBE-DONE"\n' | $VMSDCL 2>&1
