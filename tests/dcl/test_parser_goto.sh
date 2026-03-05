#!/bin/bash
# TEST: GOTO label branching in command procedure
# EXPECT: contains:AFTER_LABEL
# EXPECT_NOT: contains:BEFORE_LABEL
VMSDCL="${VMSDCL:-vmsdcl}"
TMPDIR=$(mktemp -d)
cat > "$TMPDIR/goto_test.com" << 'ENDSCRIPT'
$ GOTO TARGET
$ WRITE SYS$OUTPUT "BEFORE_LABEL"
$ TARGET:
$ WRITE SYS$OUTPUT "AFTER_LABEL"
$ EXIT
ENDSCRIPT
printf '@"%s/goto_test.com"\n' "$TMPDIR" | $VMSDCL 2>&1
rm -rf "$TMPDIR"
