#!/bin/bash
# build-tcpip-kit.sh (rd vms-f394) -- pack the TCP/IP Services layered product into a
# DISTINCT OVMX product kit (ovmx-tcpip.kit), separate from the base-OS ovmx-os.kit.
#
# On real OpenVMS, TCP/IP Services is a LAYERED PRODUCT with its own PCSI/PRODUCT kit
# installed on top of the base OS -- not part of the base-OS image. This builds the
# OVMX equivalent using the SAME factory tool the base OS kit uses (tools/ovmx_kit_pack.c),
# packing ONLY the TCP/IP Services components: the auxiliary server (TCPIP$INETD.EXE),
# its first service (TCPIP$DAYTIME.EXE), the core config tool (TCPIP$CONFIG.EXE), the
# config/startup command procedures, and the (shipped-DISABLED) service database. SSH
# (VMSSSHD) bundles into this kit as a later component (rd vms-cb0), not here.
#
# This is the PRODUCT half of vms-f394 (build + verify the kit artifact) -- reusable by
# both the tcpip_kit_roundtrip ctest and the release-stream wiring. Wiring the kit into
# the release stream as a first-class deliverable, and PRODUCT-INSTALL onto a booted
# system, are a separate rung + the layered-product-vs-appliance posture gate.
#
# Usage: build-tcpip-kit.sh <kit-out> <bin-dir> <rootfs-dir> [product-name]
#   <bin-dir>      dir with the built TCP/IP binaries + ovmx_kit_pack (e.g. build/bin)
#   <rootfs-dir>   distro/rootfs (source of the .COM procedures + TCPIP$SERVICE.DAT)
#   product-name   ovmx_kit_pack product-name literal (default "X86VMS TCPIP"; the
#                  caller varies the <arch>VMS prefix per target -- AXPVMS/VAXVMS/...)
set -eu

KIT_OUT="${1:?usage: build-tcpip-kit.sh <kit-out> <bin-dir> <rootfs-dir> [product-name]}"
BIN="${2:?bin-dir}"
ROOTFS="${3:?rootfs-dir}"
PRODUCT_NAME="${4:-X86VMS TCPIP}"

PACK="$BIN/ovmx_kit_pack"
[ -x "$PACK" ] || { echo "FATAL: ovmx_kit_pack not found/executable at $PACK (BUILD_TOOLS off?)" >&2; exit 1; }

RFS_EXE="$ROOTFS/vms/SYS0/SYSCOMMON/SYSEXE"
RFS_MGR="$ROOTFS/vms/SYS0/SYSCOMMON/SYSMGR"
RFS_STARTUP="$ROOTFS/vms/SYS0/SYSCOMMON/SYS\$STARTUP"

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/ovmx-tcpip-kit-stage.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/SYSEXE" "$STAGE/SYSMGR" "$STAGE/SYS\$STARTUP"

# Built TCP/IP component images ($-in-name -> single-quote so the shell never expands).
for b in 'TCPIP$INETD.EXE' 'TCPIP$DAYTIME.EXE' 'TCPIP$CONFIG.EXE'; do
    [ -f "$BIN/$b" ] || { echo "FATAL: TCP/IP component $b not built at $BIN" >&2; exit 1; }
    cp "$BIN/$b" "$STAGE/SYSEXE/$b"
done

# Config/startup procedures + the shipped-DISABLED service database (posture: the kit
# ships every service disabled; enabling one is an operator call, guarded by
# tests/integration/test_tcpip_posture_guard.sh).
cp "$RFS_MGR/TCPIP\$CONFIG.COM"       "$STAGE/SYSMGR/TCPIP\$CONFIG.COM"
cp "$RFS_STARTUP/TCPIP\$STARTUP.COM"  "$STAGE/SYS\$STARTUP/TCPIP\$STARTUP.COM"
# TCPIP$STARTUP.COM @-calls SYS$STARTUP:TCPIP$REAPPLY to reapply the persisted
# config at startup (rd vms-b97), so the kit MUST carry it -- otherwise a
# kit-installed TCP/IP would hit a missing procedure on startup.
cp "$RFS_STARTUP/TCPIP\$REAPPLY.COM"  "$STAGE/SYS\$STARTUP/TCPIP\$REAPPLY.COM"
cp "$RFS_EXE/TCPIP\$SERVICE.DAT"      "$STAGE/SYSEXE/TCPIP\$SERVICE.DAT"

"$PACK" pack "$KIT_OUT" "$STAGE" "$PRODUCT_NAME"
echo "OK: built TCP/IP Services layered-product kit at $KIT_OUT ($PRODUCT_NAME)"
