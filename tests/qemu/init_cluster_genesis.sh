#!/bin/busybox sh
# init_cluster_genesis.sh - PID 1 for a node of the 2-node cluster-GENESIS rig
# (rd vms-f6b).
#
# THE LIGHT PATH. This guest has no OVMX userland, no ODS-2 system disk and no
# STARTUP.EXE: it is a stock Linux kernel + busybox that loads the REAL
# executive (vms.ko) and then issues exactly the two ioctls STARTUP.EXE's
# cluster boot step issues -- VMS_IOCTL_SYSGEN_LOAD then
# VMS_IOCTL_CLUSTER_START. Everything after that (PEA0: on the virtio NIC, the
# 0x6007 HELLO cadence, SCS, the connection manager, the discovery window and
# genesis) happens INSIDE the executive, on the executive's own L2 socket. No
# userspace daemon touches the wire -- the retired SCSD.EXE is exactly what this
# rig is built without.
#
# The SYSGEN parameters come off the kernel command line (ovmx.*), which is this
# guest's stand-in for SYS$SYSTEM:OVMXVMSSYS.PAR. The rig deliberately gives
# node A VOTES and node B none, so the founding decision is a CONFIGURATION the
# executive honours rather than something this script asserts.
#
# Output: ttyS0 = console (incl. the executive's own %CNXMAN lines via printk),
#         ttyS1 = the RIG-* markers the host verdict greps + the vms dmesg tail,
#         ttyS2 = base64 pcap of every 0x6007 frame this node SAW (passive).

/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

cmdline_val() {
	# $1 = ovmx.<key>; prints the value or nothing.
	sed -n "s/.*$1=\([^ ]*\).*/\1/p" /proc/cmdline
}

TAG=$(cmdline_val ovmx.tag)
SCSNODE=$(cmdline_val ovmx.scsnode)
SYSID=$(cmdline_val ovmx.sysid)
VOTES=$(cmdline_val ovmx.votes)
EXPVOTES=$(cmdline_val ovmx.expected_votes)
VAXCLUSTER=$(cmdline_val ovmx.vaxcluster)
GROUP=$(cmdline_val ovmx.group)
RECNX=$(cmdline_val ovmx.recnx)
CREDITS=$(cmdline_val ovmx.credits)
SWVER=$(cmdline_val ovmx.swver)
WINDOW=$(cmdline_val ovmx.window)
# rd vms-94c: run the CROSS-NODE phase after the membership window, and how
# long to stay up afterwards so the PEER's op-0x03/op-0x04 land while this
# node is still running and can be read.
XNODE=$(cmdline_val ovmx.xnode)
LINGER=$(cmdline_val ovmx.linger)

[ -z "$TAG" ] && TAG=X
[ -z "$VAXCLUSTER" ] && VAXCLUSTER=2
[ -z "$GROUP" ] && GROUP=0
[ -z "$RECNX" ] && RECNX=20
[ -z "$CREDITS" ] && CREDITS=32
[ -z "$SWVER" ] && SWVER=OVMX0.6
[ -z "$WINDOW" ] && WINDOW=90
[ -z "$XNODE" ] && XNODE=0
[ -z "$LINGER" ] && LINGER=30

echo ""
echo "=== OVMX cluster GENESIS rig: node $TAG ($SCSNODE/$SYSID) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

# ---- the NIC the executive will discover -------------------------------------
# vms.ko probes the host's primary non-loopback Ethernet device at module init
# (vms_devtab_probe_nic -> exec_netdev_primary) and PEA0: binds THAT interface.
# So the link must exist and be up BEFORE insmod.
ip link set eth0 up 2>/dev/null || ifconfig eth0 up 2>/dev/null
sleep 1
MAC=$(cat /sys/class/net/eth0/address 2>/dev/null)
echo "eth0 mac=$MAC"

# ---- passive wire capture (never a transmitter) ------------------------------
# recv-only: this rig must never put a frame on the segment that the executive
# did not build. The pcap is evidence, not traffic.
# The capture must outlive everything the node does, or the very frames the
# run exists to catch fall outside it. The cross-node phase (rd vms-94c) runs
# AFTER the membership window and is followed by the linger, so its budget is
# added explicitly rather than left to the old window+20 slack.
CAPSECS=$((WINDOW + 20))
[ "$XNODE" = "1" ] && CAPSECS=$((WINDOW + LINGER + 220))
if [ -x /bin/sca_l2probe ]; then
	sca_l2probe recv eth0 "$CAPSECS" /tmp/$TAG.pcap >/tmp/cap.log 2>&1 &
fi

# ---- load the real executive -------------------------------------------------
insmod /lib/modules/vms.ko || {
	echo "RIG-$TAG-FATAL insmod vms.ko failed" > /dev/ttyS1
	sync; poweroff -f
}
sleep 1
if [ ! -c /dev/vms ]; then
	echo "RIG-$TAG-FATAL /dev/vms absent after insmod" > /dev/ttyS1
	sync; poweroff -f
fi
echo "vms.ko loaded; /dev/vms present"

# ---- SYSBOOT's two calls, then poll the executive ----------------------------
cluster_node \
	--tag="$TAG" \
	--scsnode="$SCSNODE" \
	--sysid="$SYSID" \
	--votes="$VOTES" \
	--expected-votes="$EXPVOTES" \
	--vaxcluster="$VAXCLUSTER" \
	--group="$GROUP" \
	--recnx="$RECNX" \
	--credits="$CREDITS" \
	--swver="$SWVER" \
	--window="$WINDOW" \
	--xnode="$XNODE" \
	--linger="$LINGER" \
	> /dev/ttyS1 2>&1

# ---- the executive's own transcript ------------------------------------------
# %CNXMAN / %PEA0 lines are pr_info from inside vms.ko: the connection manager's
# own account of what it did, beside the ioctl readback of what it holds.
{
	echo "===DMESG-$TAG-BEGIN==="
	dmesg | grep -a -E 'vms:|CNXMAN|PEA0|SCS' | tail -n 200
	echo "===DMESG-$TAG-END==="
} > /dev/ttyS1 2>&1

sleep 1
if [ -f /tmp/$TAG.pcap ]; then
	{
		echo "===PCAP-$TAG-B64-BEGIN==="
		base64 /tmp/$TAG.pcap
		echo "===PCAP-$TAG-B64-END==="
	} > /dev/ttyS2 2>&1
fi

sync
poweroff -f
