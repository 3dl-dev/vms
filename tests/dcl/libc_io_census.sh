#!/bin/bash
# Count the libc I/O call sites per src/vmsdcl source file (libc-drop census):
# fopen/fdopen/freopen/fgets/opendir/popen/dup and bare open(/read(/write(
# calls, and fprintf to stdout/stderr. Comment lines are not counted.
# Usage: libc_io_census.sh <repo-root>
ROOT="${1:-.}"
cd "$ROOT" || exit 2
for f in src/vmsdcl/*.c; do
    n=$(grep -E '\b(fopen|fdopen|freopen|fgets|opendir|popen|dup2?)\s*\(|fprintf\s*\(\s*(stdout|stderr)|(^|[^_a-zA-Z0-9.>$])(open|read|write)\s*\(' "$f" |
        grep -vE '^\s*(\*|//|/\*)' | wc -l)
    [ "$n" -gt 0 ] && echo "$(basename "$f") $n"
done
exit 0
