#!/bin/bash
# TEST: DCL does no new I/O through libc: the file verbs and @ have none, output goes only through the dcl_out.c sink, input only through dcl_in.c, and no file's libc call count grows (libc-drop ratchet)
# EXPECT: contains:LIBC_IO_RATCHET_OK
# EXPECT_NOT: contains:LIBC_IO_RATCHET_FAIL
#
# Negative control for libc drops (vms-28db, vms-20c94, vms-3381, vms-539, vms-f8cd):
# OPEN/READ/WRITE/CLOSE (dcl_cmd_io.c), @-procedure execution (dcl_script.c's
# procedure open), HELP's library reads (dcl_help.c, dcl_help_acp.c),
# BACKUP's files and savesets (dcl_backup.c) and LIBRARY's libraries, source
# modules, objects and /OUTPUT files (dcl_library.c) must reach files only through RMS
# and the terminal only through $QIO; a stdio
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
# HELP reads its libraries, BACKUP its files and savesets, and LIBRARY its
# libraries, modules and objects only through RMS (vms-3381, vms-539,
# vms-f8cd): no stdio stream, directory stream, or host stat/mkdir in any,
# and none in the HELP engine's RMS seam (dcl_help_acp.c).
for f in dcl_help.c dcl_backup.c dcl_help_acp.c dcl_library.c; do
    for pat in '\b(fopen|fdopen|freopen|fgets|fread|fwrite|fseek|ftell|getline|getc|fgetc)\s*\(' \
               '\b(opendir|readdir|scandir|stat|lstat|fstat|mkdir|unlink)\s*\(' \
               '(^|[^_a-zA-Z0-9.>$])(open|read|write|close)\s*\('; do
        hits=$(grep -nE "$pat" "$ROOT/src/vmsdcl/$f" | grep -vE '^[0-9]+:\s*(\*|//|/\*)')
        [ -n "$hits" ] && { echo "$f: libc file I/O is back: $hits"; bad=1; }
    done
done
# One output sink (vms-d4a0): SYS$OUTPUT and SYS$ERROR are written only by
# dcl_out.c. Any other file writing stdio's stdout/stderr bypasses the sink.
for f in "$ROOT"/src/vmsdcl/*.c; do
    [ "$(basename "$f")" = dcl_out.c ] && continue
    hits=$(grep -nE '(^|[^_a-zA-Z0-9.>$])(printf|puts|putchar|vprintf)\s*\(|\b(fputs|fputc|putc|fprintf|fwrite|vfprintf|fflush|setvbuf|ferror|clearerr)\s*\(.*\b(stdout|stderr)\b|=\s*(stdout|stderr)\b|,\s*(stdout|stderr)\s*\)' "$f" |
        grep -vE '^[0-9]+:\s*(\*|//|/\*)')
    [ -n "$hits" ] && { echo "$(basename "$f"): writes stdout/stderr outside the sink (dcl_out.c): $hits"; bad=1; }
done
# One input source (rd vms-4d27): SYS$INPUT is read only by dcl_in.c -- a
# mailbox by $QIO, a file by RMS, the terminal by its driver -- and its one
# marked stdio fallback. Any other file reading stdin bypasses it.
for f in "$ROOT"/src/vmsdcl/*.c; do
    [ "$(basename "$f")" = dcl_in.c ] && continue
    hits=$(grep -nE '\b(fgets|fread|fscanf|getline|getdelim|getc|fgetc|ungetc|feof|clearerr)\s*\(.*\bstdin\b|(^|[^_a-zA-Z0-9.>$])(getchar|gets|scanf)\s*\(|\bread\s*\(\s*(0|STDIN_FILENO)\s*,' "$f" |
        grep -vE '^[0-9]+:\s*(\*|//|/\*)')
    [ -n "$hits" ] && { echo "$(basename "$f"): reads stdin outside the input source (dcl_in.c): $hits"; bad=1; }
done
# dcl_in.c's stdio is the one block between its THE STDIO FALLBACK markers.
infb=$(awk '/THE STDIO FALLBACK/{f=1} f&&/^ \* =+ \*\/$|^\/\* =+ \*\/$/{n++; if(n==2){f=0}} !f&&/(^|[^A-Za-z0-9_])stdin([^A-Za-z0-9_]|$)/{print FNR": "$0}' "$ROOT/src/vmsdcl/dcl_in.c" | grep -vE '^[0-9]+:\s*(\*|//|/\*)')
[ -n "$infb" ] && { echo "dcl_in.c: stdin used outside its marked fallback block: $infb"; bad=1; }
# Files whose libc I/O reached 0 stay at 0 (hard pin; the ratchet alone would
# accept a baseline edit back up).
for z in dcl_io.c dcl_main.c dcl_cmd_misc.c dcl_editor.c; do
    n=$(bash "$ROOT/tests/dcl/libc_io_census.sh" "$ROOT" | awk -v f="$z" '$1==f{print $2}')
    [ -n "$n" ] && { echo "$z: $n libc I/O sites; it is pinned at 0 (rd vms-4d27, vms-67a, vms-6d6c, vms-07ae)"; bad=1; }
done
# /PAGE gathers SYS$OUTPUT's records in the sink (rd vms-07ae): the pager
# (dcl_terminal.c) and EDT's command line (dcl_editor.c) never point fd 1 elsewhere.
hits=$(grep -nE '\bdup2\s*\([^,]*,\s*(1|STDOUT_FILENO)\s*\)|\bdup\s*\(\s*(1|STDOUT_FILENO)\s*\)' "$ROOT"/src/vmsdcl/dcl_terminal.c "$ROOT"/src/vmsdcl/dcl_editor.c | grep -vE '^[^:]+:[0-9]+:\s*(\*|//|/\*)')
[ -n "$hits" ] && { echo "the /PAGE pager captures output by redirecting fd 1: $hits"; bad=1; }
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
