#!/bin/bash
# TEST: Script parameter substitution with P1 and P2
# EXPECT: contains:ALPHA
# EXPECT: contains:BETA
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
TDIR=$(mktemp -d)
printf '$ WRITE SYS$OUTPUT '"'"'P1'"'"'\n$ WRITE SYS$OUTPUT '"'"'P2'"'"'\n$ EXIT\n' > "$TDIR/params.com"
printf 'DEFINE TESTDIR "%s"\nSET DEFAULT TESTDIR:[000000]\n@params.com ALPHA BETA\n' "$TDIR" | $VMSDCL 2>&1
rm -rf "$TDIR"
