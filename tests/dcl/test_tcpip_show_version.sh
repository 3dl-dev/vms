#!/bin/bash
# TEST: TCPIP SHOW VERSION displays version string
# EXPECT: contains:OVMX TCP/IP Services for OpenVMS V0.1
VMSDCL="${VMSDCL:-vmsdcl}"
echo "TCPIP SHOW VERSION" | $VMSDCL 2>&1
