#!/bin/bash
# TEST: DIRECTORY command lists files
# EXPECT: regex:(Directory|Total of|files)
# EXPECT_NOT: regex:^(ls:|total [0-9])
VMSDCL="${VMSDCL:-vmsdcl}"
echo "DIRECTORY" | $VMSDCL 2>&1
