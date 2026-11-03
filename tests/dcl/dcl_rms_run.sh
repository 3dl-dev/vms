#!/bin/bash
# dcl_rms_run - run DCL with SYS$OUTPUT/SYS$ERROR assigned to an RMS file, and
# hand its records back read through RMS (rd vms-a6ce).
#
# The host DCL test harness (run_dcl_tests.sh) gives each test this in place of
# DCL.EXE as $VMSDCL. A test runs `... | $VMSDCL 2>&1` exactly as it always
# did; what it reads back is no longer DCL's stdio stream but the records of
# DCL's process-permanent output file:
#   1. DCL starts as a process whose creator named its SYS$OUTPUT and SYS$ERROR
#      (DCL.EXE --output SPEC --error SPEC, the $CREPRC output/error arguments
#      a host creator has no executive to pass) with its substrate stdout and
#      stderr on the null device, so dcl_out.c's FILE route $PUTs every record
#      into SPEC through RMS -- the only way out of the process there is.
#   2. rms_read_records $OPENs SPEC and $GETs the records back, one line each:
#      SYS$OUTPUT's to this script's stdout, SYS$ERROR's to its stderr.
# When the caller's stdout and stderr are one stream (2>&1), SYS$ERROR names
# SYS$OUTPUT's file, as on VMS when both are assigned to one log: the records
# keep the order DCL wrote them in. stdin passes through untouched. The exit
# status is DCL's.
#
# FAIL-LOUD. If the file was never created (DCL did not take its records to
# RMS) or a $GET fails, the reader's status goes to stderr and this exits 70
# whatever DCL returned: a test cannot pass on output that did not come
# through the file route.
#
# Environment: DCL_RMS_REAL = the DCL image (required).
set -u

real="${DCL_RMS_REAL:?dcl_rms_run: DCL_RMS_REAL (the DCL image) is not set}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
reader="${DCL_RMS_READER:-$here/rms_read_records}"
[ -x "$reader" ] || { echo "dcl_rms_run: no RMS reader at $reader" >&2; exit 70; }

# The output files live in one harness directory on the system volume (the
# host tree DCL's SYS$SYSDEVICE is: VMS_ROOT when the test sets one).
root="${VMS_ROOT:-/vms}"
hdir="DCL_RMS_OUT"
mkdir -p "$root/$hdir" 2>/dev/null
tag="O$$_${RANDOM}${RANDOM}"
out_spec="SYS\$SYSDEVICE:[$hdir]$tag.LIS"
err_spec="SYS\$SYSDEVICE:[$hdir]${tag}E.LIS"
one_stream=0
[ /proc/self/fd/1 -ef /proc/self/fd/2 ] && { one_stream=1; err_spec="$out_spec"; }

# (the host volume keeps lower-case names with ;version and RMS metadata)
cleanup() {
    local t
    for t in "$tag" "${tag,,}"; do
        rm -f "$root/$hdir/$t".[Ll][Ii][Ss]* "$root/$hdir/$t"[Ee].[Ll][Ii][Ss]* 2>/dev/null
    done
}
trap cleanup EXIT

"$real" --output "$out_spec" --error "$err_spec" "$@" >/dev/null 2>/dev/null
rc=$?

bad=0
"$reader" "$out_spec" || bad=1
if [ "$one_stream" = 0 ]; then
    "$reader" "$err_spec" >&2 || bad=1
fi
[ "$bad" = 0 ] || { echo "dcl_rms_run: DCL's records were not read back through RMS" >&2; exit 70; }
exit "$rc"
