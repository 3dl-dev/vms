#!/bin/bash
# TEST (rd vms-87e): TCPIP SET ROUTE reports its outcome HONESTLY -- it must not
# claim "route added" when the live routing table was never touched. Run here
# with no executive (the host rig: no /dev/vms), the verb cannot apply the route
# to the live table -- the executive applies it, for a SYSPRV/BYPASS caller (rd
# vms-137e) -- so it must say why (%TCPIP-W-NOSUCHDEV) and that it did not
# (%TCPIP-W-NOTAPPLIED), and NOT print the false "route added" it used to print
# unconditionally. The privilege refusal (%TCPIP-W-PRIVREQ) is the executive's,
# proved booted in tests/qemu/test_syssvc_netcfg.c. This is
# the INV-6 facade fix (the DCL sibling of the vms-f00 INETD pre-flight): the
# earlier code did `(void)system("ip route replace ... 2>/dev/null")`, discarded
# the result, and always printed "%TCPIP-I-INFO, route added".
#
# EXPECT: contains:NOSUCHDEV
# EXPECT: contains:NOTAPPLIED
# EXPECT_NOT: contains:route added
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'TCPIP SET ROUTE /DEFAULT /GATEWAY=192.0.2.1\n' | $VMSDCL 2>&1
