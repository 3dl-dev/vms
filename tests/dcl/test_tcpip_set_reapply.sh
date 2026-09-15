#!/bin/bash
# TEST (rd vms-b679, P2): the /REAPPLY qualifier makes the TCPIP SET verbs
# APPLY-ONLY -- they perform the live effect but do NOT re-write the persisted
# .DAT store. This is the apply/persist split that lets the boot reapply
# (TCPIP$REAPPLY.COM, P3) re-apply saved config at startup WITHOUT the store
# growing by a duplicate record every boot.
#
# Proof via SET ROUTE's honest non-root reporting (the CI runner is non-root):
#   - a NORMAL SET ROUTE persists -> "route recorded in TCPIP$ROUTE.DAT ..."
#   - SET ROUTE /REAPPLY is apply-only -> "not recorded (/REAPPLY)"
# The "/REAPPLY" line is produced ONLY when the persist was deliberately skipped,
# so its presence proves the store was not written (no growth on reapply). The
# full read+reapply round-trip over the ACP + the store-does-not-grow-across-reboot
# assertion are the boot e2e's (P3, vms-b97) -- the DCL reader reads SYS$SYSTEM:
# over the Files-11 ACP, which only round-trips against a real /dev/vms.
#
# EXPECT: contains:recorded in TCPIP$ROUTE.DAT
# EXPECT: contains:not recorded (/REAPPLY)
VMSDCL="${VMSDCL:-vmsdcl}"
echo "--- normal SET ROUTE persists (records the route in TCPIP\$ROUTE.DAT) ---"
printf 'TCPIP SET ROUTE /DEFAULT /GATEWAY=10.0.2.2\n' | $VMSDCL 2>&1
echo "--- SET ROUTE /REAPPLY is apply-only (does NOT record) ---"
printf 'TCPIP SET ROUTE /DEFAULT /GATEWAY=10.0.2.2 /REAPPLY\n' | $VMSDCL 2>&1
