#!/bin/bash
# TEST: RENAME command renames a file successfully
# EXPECT: contains:RENAMED_FILE.TXT
# EXPECT_NOT: contains:rename failed
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
TDIR=$(mktemp -d)
echo "rename test content" > "$TDIR/original.txt"
printf 'DEFINE TESTDIR "%s"\nSET DEFAULT TESTDIR:[000000]\nRENAME original.txt renamed_file.txt\nDIRECTORY renamed_file.txt\n' "$TDIR" | $VMSDCL 2>&1
rm -rf "$TDIR"
