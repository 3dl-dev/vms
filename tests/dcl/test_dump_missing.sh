#!/bin/bash
# TEST: DUMP of a file that does not exist is %RMS-E-FNF, not an empty dump (vms-5303e control)
# EXPECT: contains:%RMS-E-FNF
VMSDCL="${VMSDCL:-vmsdcl}"
# The default directory is a real system-disk directory: DUMP reaches files
# only through RMS (libc-drop vms-0504), so the missing FILE is what fails.
TNAME="dcl_dmp_$$"
VDIR="$(echo "$TNAME" | tr a-z A-Z)"
mkdir -p "/vms/$TNAME"
trap 'rm -rf "/vms/$TNAME"' EXIT
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nDUMP NO_SUCH_FILE_%s.DAT\n' "$VDIR" "$$" | $VMSDCL 2>&1
