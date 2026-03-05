#!/bin/bash
# TEST: Script GOTO skips to label, code before label not executed
# EXPECT: contains:AFTER_LABEL
# EXPECT_NOT: contains:BEFORE_LABEL
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
TDIR=$(mktemp -d)
printf '$ GOTO SKIP\n$ WRITE SYS$OUTPUT "BEFORE_LABEL"\n$ SKIP:\n$ WRITE SYS$OUTPUT "AFTER_LABEL"\n$ EXIT\n' > "$TDIR/labels.com"
printf 'DEFINE TESTDIR "%s"\nSET DEFAULT TESTDIR:[000000]\n@labels.com\n' "$TDIR" | $VMSDCL 2>&1
rm -rf "$TDIR"
