#!/bin/bash
# TEST: DCL does no new I/O through libc: the file verbs and @ have none, and no file's libc call count grows (libc-drop ratchet)
# EXPECT: contains:LIBC_IO_RATCHET_OK
# EXPECT_NOT: contains:LIBC_IO_RATCHET_FAIL
#
# Negative control for libc drops (vms-28db, vms-20c94): OPEN/READ/WRITE/CLOSE
# (dcl_cmd_io.c) and @-procedure execution (dcl_script.c's procedure open) must
# reach files only through RMS and the terminal only through $QIO; a stdio
# stream coming back in either fails here. Every other file's count is pinned
# to tests/dcl/libc_io_baseline.txt and may only shrink. A count below its
# baseline is reported, not failed, so PRs that remove sites concurrently in
# the merge queue do not break each other; any growth fails.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
bad=0
for pat in 'fopen\s*\(' 'fdopen\s*\(' 'freopen\s*\(' 'fgets\s*\(' '\bdup2?\s*\('; do
    hits=$(grep -nE "$pat" "$ROOT/src/vmsdcl/dcl_cmd_io.c" | grep -vE '^[0-9]+:\s*(\*|//|/\*)')
    [ -n "$hits" ] && { echo "dcl_cmd_io.c: libc stdio is back: $hits"; bad=1; }
done
hits=$(grep -nE 'fopen\s*\(' "$ROOT/src/vmsdcl/dcl_script.c" | grep -vE '^[0-9]+:\s*(\*|//|/\*)')
[ -n "$hits" ] && { echo "dcl_script.c: @ opens a procedure with fopen: $hits"; bad=1; }
base="$ROOT/tests/dcl/libc_io_baseline.txt"
now=$(bash "$ROOT/tests/dcl/libc_io_census.sh" "$ROOT")
while read -r f n; do
    b=$(grep -v '^#' "$base" | awk -v f="$f" '$1==f{print $2}')
    b=${b:-0}
    if [ "$n" -gt "$b" ]; then echo "$f: $n libc I/O sites, baseline $b (a new libc drop)"; bad=1
    elif [ "$n" -lt "$b" ]; then echo "note: $f: $n libc I/O sites, below baseline $b -- lower it"; fi
done <<< "$now"
grep -v '^#' "$base" | while read -r f b; do
    printf '%s\n' "$now" | grep -q "^$f " || echo "note: $f: 0 libc I/O sites, baseline $b -- drop the line"
done
if [ "$bad" = 0 ]; then echo LIBC_IO_RATCHET_OK; else echo LIBC_IO_RATCHET_FAIL; fi
