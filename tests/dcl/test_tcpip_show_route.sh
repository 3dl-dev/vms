#!/bin/bash
# TEST: TCPIP SHOW ROUTE with no executive fails honestly, never lists the host's routing table
# The routing table comes FROM THE EXECUTIVE (an IO$_SENSEMODE route-list
# request on a BGn: channel, rd vms-6d6c) -- as on VMS, where the TCP/IP kernel
# component owns the routes. This harness runs DCL.EXE with no /dev/vms, so the
# command must say the executive is absent and exit SS$_NOSUCHDEV (%X00000908)
# -- not fall back to /proc/net/route (which printed the HOST's 'default ...
# eth0', rd vms-5fe9). The executive-present listing (SE0's on-link route, a
# default route via a gateway on SE0) is proven against a real vms.ko by
# tests/qemu/test_syssvc_dcl_tcpip_iflist.c and test_tcpip_external_boot_e2e.sh.
# EXPECT: contains:%TCPIP-E-NOEXEC, executive absent -- the routing table cannot be read
# EXPECT: contains:$STATUS == "%X00000908"
# EXPECT_NOT: contains:Destination
# EXPECT_NOT: contains:default
# EXPECT_NOT: contains:eth0
# EXPECT_NOT: regex:\bens[0-9]
# EXPECT_NOT: regex:\benp[0-9]
# EXPECT_NOT: regex:\bdocker[0-9]
VMSDCL="${VMSDCL:-vmsdcl}"
if [ -e /dev/vms ]; then
    echo "test_tcpip_show_route: /dev/vms is present -- this is the no-executive case; run without vms.ko"
    exit 1
fi
printf 'TCPIP SHOW ROUTE\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
