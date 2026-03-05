#!/bin/bash
# TEST: TCPIP SHOW ROUTE displays routing table with VMS device names
# EXPECT: contains:Destination
# EXPECT: contains:Gateway
# EXPECT: regex:(SE[0-9]|LO[0-9]|default)
# EXPECT_NOT: contains:eth0
# EXPECT_NOT: regex:\bens[0-9]
# EXPECT_NOT: regex:\benp[0-9]
VMSDCL="${VMSDCL:-vmsdcl}"
echo "TCPIP SHOW ROUTE" | $VMSDCL 2>&1
