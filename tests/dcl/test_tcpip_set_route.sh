#!/bin/bash
# TEST (rd vms-87e): TCPIP SET ROUTE reports its outcome HONESTLY -- it must not
# claim "route added" when the live routing table was never touched. Run without
# NET_ADMIN privilege (the CI runner is non-root), the verb cannot apply the
# route to the live table, so it must say so (%TCPIP-W-PRIVREQ + %TCPIP-W-NOTAPPLIED)
# and NOT print the false "route added" it used to print unconditionally. This is
# the INV-6 facade fix (the DCL sibling of the vms-f00 INETD pre-flight): the
# earlier code did `(void)system("ip route replace ... 2>/dev/null")`, discarded
# the result, and always printed "%TCPIP-I-INFO, route added".
#
# EXPECT: contains:PRIVREQ
# EXPECT: contains:NOTAPPLIED
# EXPECT_NOT: contains:route added
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'TCPIP SET ROUTE /DEFAULT /GATEWAY=192.0.2.1\n' | $VMSDCL 2>&1
