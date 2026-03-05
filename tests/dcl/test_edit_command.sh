#!/bin/bash
# TEST: EDIT command launches EDT editor, creates and modifies a file
# EXPECT: contains:EDT_TEST_LINE_ONE
# EXPECT: contains:EDT_MODIFIED_LINE
# EXPECT: contains:EDT_TEST_LINE_THREE
# EXPECT_NOT: contains:EDT_TEST_LINE_TWO
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_edittest_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$VDIR"

# Test 1: Create a file via EDIT with INSERT, then EXIT (saves)
# All commands go through single stdin pipe: DCL commands first, then EDT commands
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nEDIT EDTFILE.TXT\nINSERT\nEDT_TEST_LINE_ONE\nEDT_TEST_LINE_TWO\nEDT_TEST_LINE_THREE\n\nEXIT\n' "$VDIR" | $VMSDCL 2>&1

# Test 2: Re-open file, delete line 2, substitute on line 1, then EXIT
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nEDIT EDTFILE.TXT\nDELETE 2\n1\nSUBSTITUTE /TEST_LINE_ONE/MODIFIED_LINE/\nEXIT\n' "$VDIR" | $VMSDCL 2>&1

# Display the resulting file to verify
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nTYPE EDTFILE.TXT\n' "$VDIR" | $VMSDCL 2>&1

rm -rf "/vms/$VDIR"
