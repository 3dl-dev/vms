#!/bin/bash
# TEST: @SYS$MANAGER:TCPIP$CONFIG does NOT falsely claim success when a core
#       configuration step fails (INV-6 honesty, rd vms-67f).
#
# The procedure used to print "%TCPIP-S-CONFIGURED, ... core configuration
# complete" UNCONDITIONALLY -- even when the host/domain logical DEFINE or the
# TCPIP SET INTERFACE failed (observed during the vms-21b daytime/SSH cold-boot
# work: it printed CONFIGURED right after "%TCPIP-E-NOSUCHDEV, unknown interface"
# and "Host address (not configured)"). That is a false success claim. The fix
# tracks each step's $SEVERITY and, if any failed, reports %TCPIP-E-CFGINCOMPLETE
# and exits with an error severity instead of claiming success.
#
# Here we drive it with an interface name the substrate cannot resolve (ZZ99 --
# TCPIP SET INTERFACE maps /sys/class/net devices to SE0/EW0/LO0/... , never ZZ99,
# so the SET is refused deterministically, independent of executive/root). The
# honest result: NO CONFIGURED claim, a reported failure. (The full happy-path
# CONFIGURED is proven on a real cold boot by tests/qemu/run_tcpip_daytime_boot_e2e.sh
# where SE0 comes up and TCPIP$CONFIG legitimately succeeds.)
#
# EXPECT: contains:TCPIP-E-CFGINCOMPLETE
# EXPECT_NOT: contains:TCPIP-S-CONFIGURED
set -u
VMSDCL="${VMSDCL:-vmsdcl}"

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRCCOM="$REPO/distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/TCPIP\$CONFIG.COM"
if [ ! -f "$SRCCOM" ]; then echo "SKIP: TCPIP\$CONFIG.COM not found"; exit 0; fi

# Make the shipped procedure resolvable as SYS$MANAGER:TCPIP$CONFIG in host mode
# (the same staging tests/dcl/test_cluster_config_lan.sh uses).
MGR=/vms/SYS0/SYSCOMMON/SYSMGR
mkdir -p "$MGR"
cp "$SRCCOM" "$MGR/TCPIP\$CONFIG.COM"

# No executive on the host CI, and ZZ99 is not a resolvable interface, so the
# DEFINEs and the interface SET fail -- the honest outcome is a reported failure,
# NOT a CONFIGURED claim.
echo '@SYS$MANAGER:TCPIP$CONFIG OVMX OVMX.LOCAL ZZ99 10.0.2.15 255.255.255.0' | "$VMSDCL" 2>&1
