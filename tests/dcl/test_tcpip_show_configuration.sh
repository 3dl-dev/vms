#!/bin/bash
# TEST: TCPIP SHOW CONFIGURATION reads the TCPIP$ SYSTEM logicals honestly.
#
# On the host CI the TCP/IP Services executive is not running, so the TCPIP$*
# SYSTEM logicals (executive-resident LNM$SYSTEM) are unreachable. The
# VMS-faithful, INV-6-honest behaviour is to SAY SO -- report %TCPIP-W-UNAVAIL
# -- never to fabricate a per-process configuration. The full set/read-back
# round trip is proven against a real executive by
# tests/qemu/test_syssvc_tcpip_config.c.
#
# EXPECT: contains:Item
# EXPECT: contains:TCPIP-W-UNAVAIL
# EXPECT_NOT: regex:[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+
VMSDCL="${VMSDCL:-vmsdcl}"
echo "TCPIP SHOW CONFIGURATION" | $VMSDCL 2>&1
