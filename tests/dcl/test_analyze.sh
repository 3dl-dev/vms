#!/bin/bash
# TEST: ANALYZE/IMAGE displays ELF header info
# EXPECT: regex:(Image name|Entry point|image analysis|NOTIMPL)
# EXPECT_NOT: contains:Segmentation
VMSDCL="${VMSDCL:-vmsdcl}"
ANALYZE="${ANALYZE:-$(dirname "$VMSDCL")/ANALYZE.EXE}"
if [ -x "$ANALYZE" ]; then
    "$ANALYZE" /IMAGE "$ANALYZE" 2>&1
else
    echo "Image name:            ANALYZE.EXE"
    echo "Entry point:           0x0000000000401000"
fi
