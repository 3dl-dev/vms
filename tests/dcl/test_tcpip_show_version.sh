#!/bin/bash
# TEST: TCPIP SHOW VERSION reports the SAME product version as an
# independently-implemented surface (SHOW SYSTEM), both drawn from the
# identity SSOT (INV-1), and does not name itself "TCP/IP Services for
# OpenVMS" (VSI's product name, INV-0)
# EXPECT: contains:VERSION_CROSSCHECK_OK
# EXPECT_NOT: contains:VERSION_CROSSCHECK_FAIL
# EXPECT_NOT: regex:TCP/IP Services for OpenVMS
#
# vms-fe21: re-armed. The old test's only assertion was a regex matching
# TCPIP SHOW VERSION's OWN printf template character-for-character
# ("OVMX TCP/IP Services V[0-9]+\.[0-9]+") -- src/vmsdcl/dcl_cmd_misc.c's
# cmd_tcpip_show_version() prints exactly that shape unconditionally, so the
# test passed by construction and could never catch a wrong, stale, or
# hardcoded-instead-of-SSOT-derived version string.
#
# The re-armed check is a drift detector: SHOW SYSTEM's header
# (dcl_cmd_show.c) and TCPIP SHOW VERSION (dcl_cmd_misc.c) are two
# separately-coded call sites that both call ovmx_product_version() /
# ovmx_product_banner() (src/libvms/include/ovmx_identity.h) -- the actual
# version SSOT. If either call site regressed to a literal hardcoded string
# instead of reading the SSOT, the two would diverge and this test would
# catch it; today, with both wired to the SSOT correctly, they must agree.
# A test that only checks TCPIP SHOW VERSION's own format can never detect
# that kind of divergence, since it never looks at anything else.
VMSDCL="${VMSDCL:-vmsdcl}"

output=$(printf 'SHOW SYSTEM\nTCPIP SHOW VERSION\n' | $VMSDCL 2>&1)
echo "$output"

sys_ver=$(echo "$output" | grep -oE 'OpenVMX V[0-9]+\.[0-9]+(-[0-9]+)?' | head -1 | grep -oE 'V[0-9]+\.[0-9]+(-[0-9]+)?')
tcpip_ver=$(echo "$output" | grep -oE 'OVMX TCP/IP Services V[0-9]+\.[0-9]+(-[0-9]+)?' | head -1 | grep -oE 'V[0-9]+\.[0-9]+(-[0-9]+)?')

if [ -z "$sys_ver" ] || [ -z "$tcpip_ver" ]; then
    echo "VERSION_CROSSCHECK_FAIL (could not extract a version from one or both surfaces: SHOW SYSTEM='$sys_ver' TCPIP='$tcpip_ver')"
    exit 1
fi

if [ "$sys_ver" = "$tcpip_ver" ]; then
    echo "VERSION_CROSSCHECK_OK (SHOW SYSTEM=$sys_ver, TCPIP SHOW VERSION=$tcpip_ver)"
else
    echo "VERSION_CROSSCHECK_FAIL (SHOW SYSTEM=$sys_ver != TCPIP SHOW VERSION=$tcpip_ver)"
    exit 1
fi
