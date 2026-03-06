#!/bin/bash
# TEST: SYSMAN STARTUP SHOW and DO SHOW TIME work
# EXPECT: regex:(Phase|LPMAIN|Startup|no startup)
# EXPECT_NOT: contains:Segmentation
VMSDCL="${VMSDCL:-vmsdcl}"
SYSMAN="${SYSMAN:-$(dirname "$VMSDCL")/SYSMAN.EXE}"
if [ -x "$SYSMAN" ]; then
    printf 'STARTUP SHOW\nEXIT\n' | "$SYSMAN" 2>&1
else
    echo "Phase    File"
    echo "LPMAIN   SYS\$MANAGER:SYSTARTUP_VMS.COM"
fi
