#!/bin/bash
# TEST: vms-f89 - DEFINE SYS$INPUT <file> redirects READ SYS$INPUT live (config
#       from logicals, parent vms-704). DCL I/O resolves SYS$INPUT through real
#       logical translation, and successive READs advance through the file.
#
# --- READ SYS$INPUT reads consecutive lines from the redirected file.
# EXPECT: contains:AA = "OVMX_F89_LINE_ONE"
# EXPECT: contains:BB = "OVMX_F89_LINE_TWO"
#
# THE FINDING THIS GATES (INV-DCL): on origin/main READ named SYS$INPUT found no
# open channel and errored (%DCL-W-IVLOGNAM), so AA/BB stayed unset -- SYS$INPUT
# was not translated at all. TRIPWIRE: revert the SYS$INPUT translation in
# cmd_read() (src/vmsdcl/dcl_cmd_io.c) and the two lines never reach the symbols,
# so both EXPECTs fail. Doc pin (VSI OpenVMS DCL Dictionary): SYS$INPUT is the
# process-permanent logical for the default input stream.
#
# The redirected file's lines are read INSTEAD of this script's piped DCL
# commands (stdin), which is the point: redefining the logical moves the source.
# A VMS filespec (SYS$SYSDEVICE:[dir]) is used, not a Linux /path -- a bare
# leading "/" is a DCL qualifier, not a file.
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_f89i_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"
printf 'OVMX_F89_LINE_ONE\nOVMX_F89_LINE_TWO\n' > "/vms/$TDIR/in.txt"
printf 'DEFINE SYS$INPUT SYS$SYSDEVICE:[%s]IN.TXT\nREAD SYS$INPUT AA\nREAD SYS$INPUT BB\nSHOW SYMBOL AA\nSHOW SYMBOL BB\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR"
