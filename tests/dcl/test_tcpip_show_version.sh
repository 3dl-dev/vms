#!/bin/bash
# TEST: TCPIP SHOW VERSION displays version string
# Version comes from the identity SSOT (INV-1), and the product no longer
# names itself "TCP/IP Services for OpenVMS" (VSI's product name, INV-0).
# EXPECT: regex:OVMX TCP/IP Services V[0-9]+\.[0-9]+
VMSDCL="${VMSDCL:-vmsdcl}"
echo "TCPIP SHOW VERSION" | $VMSDCL 2>&1
