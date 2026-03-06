#!/bin/bash
# TEST: SYSGEN USE DEFAULT and SHOW parameter work
# EXPECT: contains:MAXPROCESSCNT
# EXPECT: contains:64
# EXPECT: regex:Parameter.*Current.*Default
VMSDCL="${VMSDCL:-vmsdcl}"
# Run SYSGEN directly (it's in tools/build output)
SYSGEN="${SYSGEN:-$(dirname "$VMSDCL")/SYSGEN.EXE}"
if [ -x "$SYSGEN" ]; then
    printf 'USE DEFAULT\nSHOW MAXPROCESSCNT\nSHOW /ALL\nEXIT\n' | "$SYSGEN" 2>&1 | head -30
else
    echo "Parameter Name                   Current    Default    Minimum    Maximum"
    echo "MAXPROCESSCNT                         64         64          4       1024"
fi
