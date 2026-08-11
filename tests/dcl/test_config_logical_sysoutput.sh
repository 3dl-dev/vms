#!/bin/bash
# TEST: vms-f89 - DEFINE SYS$OUTPUT <file> redirects WRITE SYS$OUTPUT live
#       (config from logicals, parent vms-704). DCL I/O resolves SYS$OUTPUT
#       through real logical translation instead of a string match, so a
#       redefinition actually redirects the stream.
#
# The markers below are COMPUTED into fixed tokens by this script (never echoed
# raw) so the harness verdict is unambiguous:
# --- After redirect, the written text is in the FILE ...
# EXPECT: contains:MARKER_IN_FILE_YES
# --- ... and did NOT go to the process stdout.
# EXPECT: contains:MARKER_ON_STDOUT_NO
#
# THE FINDING THIS GATES (docs/design-dcl-fidelity.md, INV-DCL): cmd_write()
# matched the literal "SYS$OUTPUT" and always printf()'d to stdout, so
# DEFINE SYS$OUTPUT <file> did nothing -- the wrong subsystem behaviour dressed
# in WRITE's shape. TRIPWIRE: on origin/main the text goes to stdout and the
# file stays empty, so both tokens flip (MARKER_ON_STDOUT_YES / MARKER_IN_FILE_NO)
# and both EXPECTs fail. Doc pin (VSI OpenVMS DCL Dictionary): SYS$OUTPUT is the
# process-permanent logical for the default output stream.
#
# A VMS filespec (SYS$SYSDEVICE:[dir]) is used, not a Linux /path -- a bare
# leading "/" is a DCL qualifier, not a file.
VMSDCL="${VMSDCL:-vmsdcl}"
MARK="OVMX_F89_SYSOUT_MARKER"
TDIR="dcl_f89o_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"

dcl_out=$(printf 'DEFINE SYS$OUTPUT SYS$SYSDEVICE:[%s]OUT.TXT\nWRITE SYS$OUTPUT "%s"\nDEASSIGN SYS$OUTPUT\n' "$VDIR" "$MARK" | $VMSDCL 2>&1)

if printf '%s' "$dcl_out" | grep -qF "$MARK"; then
    echo "MARKER_ON_STDOUT_YES"
else
    echo "MARKER_ON_STDOUT_NO"
fi

if grep -rqF "$MARK" "/vms/$TDIR" 2>/dev/null; then
    echo "MARKER_IN_FILE_YES"
else
    echo "MARKER_IN_FILE_NO"
fi

rm -rf "/vms/$TDIR"
