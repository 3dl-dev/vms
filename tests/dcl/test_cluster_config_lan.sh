#!/bin/bash
# TEST: @SYS$MANAGER:CLUSTER_CONFIG_LAN.COM authors a node's cluster identity the
#       VMS way (vms-258) -- the operator picks CHANGE, answers SCSNODE/
#       SCSSYSTEMID/..., and the procedure drives SYSGEN (USE/SET/WRITE CURRENT)
#       so the new identity lands in the OVMXVMSSYS.PAR store the system reads.
#       Also guards the honest deferrals (Rule 10 / INV-6) and input validation.
#
# Reference (clean-room, Rule 8): VSI OpenVMS Cluster Systems manual --
# CLUSTER_CONFIG_LAN.COM operator procedure. OVMX authors all .COM content.
#
# The confirmation screen echoes the values the operator entered.
# EXPECT: regex:SCSNODE +. +OVMXA
# EXPECT: regex:SCSSYSTEMID +. +1030
# SYSGEN actually set the params (driven through the fixed RUN SYS$INPUT path).
# EXPECT: contains:SCSNODE changed from OVMX to OVMXA
# EXPECT: contains:SCSSYSTEMID changed from 0 to 1030
# The identity is read back from the store with NO quote characters (vms-4fe).
# EXPECT: regex:READBACK SCSNODE=OVMXA$
# EXPECT: contains:READBACK SCSSYSTEMID=1030
# An over-long SCSNODE is rejected, not written.
# EXPECT: contains:CLUSTER_CONFIG-E-BADNODE
# REMOVE is honestly declined, never faked (Rule 10).
# EXPECT: contains:not available at this OpenVMX
set -u
VMSDCL="${VMSDCL:-vmsdcl}"

bindir=""
case "$VMSDCL" in */*) bindir="$(cd "$(dirname "$VMSDCL")" && pwd)";; esac
SYSGEN=""
for c in "$bindir/SYSGEN.EXE" ./build/bin/SYSGEN.EXE ../build/bin/SYSGEN.EXE; do
    [ -x "$c" ] && { SYSGEN="$c"; break; }
done
if [ -z "$SYSGEN" ]; then echo "SKIP: SYSGEN.EXE not found"; exit 0; fi

# The shipped procedure under test.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRCCOM="$REPO/distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/CLUSTER_CONFIG_LAN.COM"
if [ ! -f "$SRCCOM" ]; then echo "SKIP: CLUSTER_CONFIG_LAN.COM not found"; exit 0; fi

EXE=/vms/SYS0/SYSCOMMON/SYSEXE
MGR=/vms/SYS0/SYSCOMMON/SYSMGR
mkdir -p "$EXE" "$MGR"
cp "$SYSGEN" "$EXE/SYSGEN.EXE"
cp "$SRCCOM" "$MGR/CLUSTER_CONFIG_LAN.COM"

STORE="$(mktemp -u /tmp/ovmx_ccl_store_XXXXXX.par)"
export OVMX_SYSGEN_PATH="$STORE"
# Seed a current store from factory defaults.
printf 'USE DEFAULT\nWRITE CURRENT\nEXIT\n' | "$SYSGEN" >/dev/null 2>&1

# Drive: menu 2 (CHANGE); a too-long name is rejected then OVMXA accepted;
# SCSSYSTEMID 1030; ALLOCLASS/VOTES/EXPECTED_VOTES defaulted; confirm Y; then
# menu 3 (REMOVE, honest decline); then menu 5 (EXIT).
printf '2\nTOOLONGNAME\nOVMXA\n1030\n\n\n\nY\n3\n5\n' \
    | "$VMSDCL" 'SYS$MANAGER:CLUSTER_CONFIG_LAN.COM' 2>&1

# Read the authored identity back out of the store. SYSGEN's SHOW renders a
# string param quoted and space-padded ("OVMXA  "); strip that display framing
# so the assertion checks the stored VALUE, not the formatting.
rb="$(printf 'USE CURRENT\nSHOW SCSNODE\nSHOW SCSSYSTEMID\nEXIT\n' | "$SYSGEN" 2>&1)"
node="$(printf '%s\n' "$rb" | grep '^  SCSNODE ' | head -1 | sed 's/[^"]*"\([^"]*\)".*/\1/' | tr -d ' ')"
sid="$(printf '%s\n' "$rb" | grep '^  SCSSYSTEMID ' | head -1 | awk '{print $2}')"
echo "READBACK SCSNODE=$node"
echo "READBACK SCSSYSTEMID=$sid"

rm -f "$STORE" "$MGR/CLUSTER_CONFIG_LAN.COM" "$EXE/SYSGEN.EXE"
