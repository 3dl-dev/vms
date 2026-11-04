#!/bin/bash
# TEST: TCPIP SHOW INTERFACE with no executive fails honestly, never lists the host's interfaces
# The interface list comes FROM THE EXECUTIVE (an IO$_SENSEMODE interface-list
# request on a BGn: channel, rd vms-99f). This harness runs DCL.EXE with no
# /dev/vms, so the command must say the executive is absent and exit
# SS$_NOSUCHDEV (%X00000908) -- not fall back to /sys/class/net or a host
# socket. The executive-present listing (LO0 127.0.0.1 255.0.0.0, SE0's
# values) is proven against a real vms.ko by
# tests/qemu/test_syssvc_dcl_tcpip_iflist.c.
# EXPECT: contains:%TCPIP-E-NOEXEC, executive absent -- the interface list cannot be read
# EXPECT: contains:$STATUS == "%X00000908"
# EXPECT_NOT: contains:IP Address
# EXPECT_NOT: regex:\bLO0\b
# EXPECT_NOT: contains:127.0.0.1
# EXPECT_NOT: contains:SOCKERR
# EXPECT_NOT: contains:eth0
VMSDCL="${VMSDCL:-vmsdcl}"
if [ -e /dev/vms ]; then
    echo "test_tcpip_show: /dev/vms is present -- this is the no-executive case; run without vms.ko"
    exit 1
fi
printf 'TCPIP SHOW INTERFACE\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
