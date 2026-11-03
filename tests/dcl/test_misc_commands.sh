#!/bin/bash
# DCL_OUTPUT: stdio vms-380f an image/PIPE segment DCL runs does not yet write the parent's file SYS$OUTPUT
# TEST: ATTACH, CONVERT, LINK, INSTALL, PRODUCT, PHONE commands exist and respond correctly
# EXPECT: contains:ATTFAIL
# EXPECT: contains:NOTAVAIL
# EXPECT: contains:OVMX
# EXPECT: contains:Installed
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'ATTACH\nPHONE\nPRODUCT SHOW PRODUCT\n' | $VMSDCL 2>&1
