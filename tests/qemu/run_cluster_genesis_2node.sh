#!/bin/bash
# run_cluster_genesis_2node.sh - host side of the 2-node cluster GENESIS rig
# (rd vms-f6b). Runs INSIDE the container built from
# tests/qemu/Dockerfile.cluster-genesis-2node.
#
# TOPOLOGY. Two minimal QEMU guests in this container's own netns, wired by a
# QEMU `socket` netdev on loopback -- one L2 segment, no bridge, no NET_ADMIN,
# no NET_RAW. The only host resource wanted is /dev/kvm (TCG fallback if
# absent, much slower). Each guest loads the REAL executive and issues
# STARTUP.EXE's two cluster ioctls; the executive does all L2 I/O.
#
# WHY listen/connect AND NOT mcast. QEMU's `socket,mcast=` netdev leaves
# IP_MULTICAST_LOOP on (that is how several guests on ONE host hear each
# other), so a guest also receives ITS OWN group multicast. Measured on the
# first run of this rig: a node booted entirely alone logged "%PEA0, channel
# verified" two seconds after PEA0: came up -- it had formed a channel with
# ITSELF off its own AB-00-04-01 HELLO. That is a segment a real LAN never
# presents (a station does not receive its own transmissions), and it makes the
# genesis measurement meaningless: a node that hears "somebody" must join
# rather than form. A point-to-point socket delivers each node's frames to the
# OTHER node only, which is the segment being modelled.
# (That the executive ACCEPTS a HELLO bearing its own SCSSYSTEMID is a separate
# robustness observation, recorded rather than worked around here.)
#
# WHY THE START IS STAGGERED, and why that is faithful rather than convenient.
# Genesis is refused while any peer connection manager is present
# (cnxman_genesis_may_ask -> cnxman_join_target_present): a node that can hear
# somebody must JOIN, not FORM. So two nodes powered on together can never
# form a cluster -- and neither can two real VAXes; a VMScluster is formed by
# booting its first member and then booting the rest. The rig reproduces that:
# node A boots alone, spends its whole RECNXINTERVAL discovery window hearing
# nobody, and founds generation 1; node B is powered on afterwards and must
# join what is already there.
#
# NODE ROLES (SYSGEN, nothing else):
#   A  OVMXA/1025  VOTES=1 EXPECTED_VOTES=1 VAXCLUSTER=2  -> quorum by own votes
#   B  OVMXB/1026  VOTES=0                 VAXCLUSTER=2  -> can never found
#
# NEGATIVE CONTROL (RIG_NEGCTL=1): node A is given VOTES=0 too. Nothing else
# changes. Neither node then satisfies quorum by its own votes, so neither may
# found, so no cluster exists to join -- the run MUST NOT reach CN=2. That is
# the teeth: it is what distinguishes a measured membership from a printed one.
#
# VERDICT: read only from the guests' own RIG-*-FINAL lines, which carry values
# the guest read back out of the executive (INV-6).

set -uo pipefail

OUT="${OUT_DIR:-/out}"
mkdir -p "$OUT"

NEGCTL="${RIG_NEGCTL:-0}"
GROUP="${RIG_GROUP:-2026}"
RECNX="${RIG_RECNX:-8}"           # RECNXINTERVAL == the genesis discovery window
# CLUSTER_CREDITS: the receive-buffer grant each port advertises at abs 95 of
# its formation body. Load-bearing: with 0 the circuit opens and the peer may
# put NOTHING on it (p. 2-43 "no credit, no message"), which is exactly what a
# first run of this rig measured -- both VCs OPEN, credits_send=0, and not one
# sequenced frame in either direction. 32 is VMS's own default.
CREDITS="${RIG_CREDITS:-32}"
STAGGER="${RIG_STAGGER:-30}"      # seconds between powering on A and B
WINDOW_A="${RIG_WINDOW_A:-150}"   # how long each node polls the executive
WINDOW_B="${RIG_WINDOW_B:-110}"
WALL="${RIG_WALL:-600}"

VOTES_A=1
[ "$NEGCTL" = "1" ] && VOTES_A=0

KERNEL=/boot/vmlinuz
INITRD=/initramfs.cpio.gz
SEGMENT_PORT=16009   # the loopback TCP port carrying the L2 segment

QEMU=qemu-system-x86_64
if [ -w /dev/kvm ]; then ACCEL="-accel kvm -cpu host"; else ACCEL="-accel tcg"; fi

echo "=== OVMX 2-node cluster GENESIS rig (rd vms-f6b) ==="
echo "mode=$( [ "$NEGCTL" = 1 ] && echo NEGATIVE-CONTROL || echo PROOF )"
echo "accel=${ACCEL#-accel } group=$GROUP recnx=${RECNX}s stagger=${STAGGER}s"
echo "node A: OVMXA/1025 VOTES=$VOTES_A EXPECTED_VOTES=1 VAXCLUSTER=2"
echo "node B: OVMXB/1026 VOTES=0          VAXCLUSTER=2"
echo ""

# --------------------------------------------------------------------------
# Launching one guest
# --------------------------------------------------------------------------

# The SYSGEN identity rides the kernel command line; the initramfs is symmetric.
node_cmdline() {
	# $1=tag $2=scsnode $3=sysid $4=votes $5=expected_votes $6=window
	echo "console=ttyS0 net.ifnames=0 biosdevname=0 panic=-1 loglevel=7" \
	     "ovmx.tag=$1 ovmx.scsnode=$2 ovmx.sysid=$3 ovmx.votes=$4" \
	     "ovmx.expected_votes=$5 ovmx.vaxcluster=2 ovmx.group=$GROUP" \
	     "ovmx.recnx=$RECNX ovmx.credits=$CREDITS ovmx.window=$6"
}

# Node A holds the segment open; node B dials in. A is powered on first
# anyway, so the listener is always up before the connector.
segment_netdev() {
	# $1 = node tag
	if [ "$1" = "A" ]; then
		echo "socket,id=net0,listen=127.0.0.1:${SEGMENT_PORT}"
	else
		echo "socket,id=net0,connect=127.0.0.1:${SEGMENT_PORT}"
	fi
}

LAUNCH_PID=0
launch_node() {
	# $1=tag $2=scsnode $3=sysid $4=votes $5=expected_votes $6=mac $7=window
	local tag="$1" mac="$6"
	local append; append=$(node_cmdline "$1" "$2" "$3" "$4" "$5" "$7")
	local netdev; netdev=$(segment_netdev "$tag")

	$QEMU $ACCEL \
		-kernel "$KERNEL" -initrd "$INITRD" \
		-append "$append" \
		-m 512M -smp 1 -nographic -no-reboot -nodefaults \
		-netdev "$netdev" \
		-device "virtio-net-pci,netdev=net0,mac=${mac},romfile=" \
		-serial "file:$OUT/node${tag}.console.log" \
		-serial "file:$OUT/node${tag}.ttyS1.log" \
		-serial "file:$OUT/node${tag}.pcap.b64" \
		>/dev/null 2>&1 &
	LAUNCH_PID=$!
}

echo "--- powering on node A (it must hear nobody for ${RECNX}s, then found) ---"
launch_node A OVMXA 1025 "$VOTES_A" 1 52:54:00:00:10:25 "$WINDOW_A"; PA=$LAUNCH_PID
sleep "$STAGGER"
echo "--- powering on node B (it must join what A formed) ---"
launch_node B OVMXB 1026 0 1 52:54:00:00:10:26 "$WINDOW_B"; PB=$LAUNCH_PID

( sleep "$WALL"; kill -9 "$PA" "$PB" 2>/dev/null ) & GUARD=$!
wait "$PA" 2>/dev/null
wait "$PB" 2>/dev/null
kill "$GUARD" 2>/dev/null

# --------------------------------------------------------------------------
# Reading the guests' executive readback
# --------------------------------------------------------------------------

for N in A B; do
	echo ""
	echo "=== node $N: what its executive held (ttyS1) ==="
	cat "$OUT/node${N}.ttyS1.log" 2>/dev/null || echo "(no output)"
done

for N in A B; do
	B64="$OUT/node${N}.pcap.b64"
	[ -s "$B64" ] || continue
	sed -n "/===PCAP-${N}-B64-BEGIN===/,/===PCAP-${N}-B64-END===/p" "$B64" \
		| grep -v '===PCAP-' | tr -d '\r' | base64 -d \
		> "$OUT/node${N}.pcap" 2>/dev/null || true
	[ -s "$OUT/node${N}.pcap" ] && \
		echo "reconstructed pcap: $OUT/node${N}.pcap ($(wc -c < "$OUT/node${N}.pcap") bytes)"
done

# `RIG-<tag>-FINAL <key>=<value>`: pull one key out of one node's verdict line.
final_field() {
	# $1=tag $2=key
	grep -a "RIG-$1-FINAL" "$OUT/node$1.ttyS1.log" 2>/dev/null | tail -n 1 \
		| tr ' ' '\n' | sed -n "s/^$2=//p" | tail -n 1
}

echo ""
echo "=== verdict inputs (each value was read out of that node's executive) ==="
A_MEMBER=$(final_field A member); A_CN=$(final_field A cn); A_ROLE=$(final_field A role)
B_MEMBER=$(final_field B member); B_CN=$(final_field B cn); B_ROLE=$(final_field B role)
A_CSID=$(final_field A csid);     B_CSID=$(final_field B csid)
printf "  node A: role=%s member=%s cn=%s csid=%s\n" \
	"${A_ROLE:-?}" "${A_MEMBER:-?}" "${A_CN:-?}" "${A_CSID:-?}"
printf "  node B: role=%s member=%s cn=%s csid=%s\n" \
	"${B_ROLE:-?}" "${B_MEMBER:-?}" "${B_CN:-?}" "${B_CSID:-?}"

cn2_reached() {
	[ "$A_MEMBER" = "1" ] && [ "$B_MEMBER" = "1" ] && \
	[ "$A_CN" = "2" ] && [ "$B_CN" = "2" ]
}

# Did ANY node assert a membership or hold a cluster system id? This is what
# the negative control forbids, and it is deliberately STRONGER than "CN != 2":
# the thing under test at this rung is GENESIS, and genesis shows up as node A
# reaching MEMBER with a minted CSID. A control that only forbade CN=2 would
# still pass on a run where a node founded a cluster it had no votes for.
anyone_claimed_membership() {
	[ "$A_MEMBER" = "1" ] || [ "$B_MEMBER" = "1" ] || \
	{ [ -n "$A_CSID" ] && [ "$A_CSID" != "-" ]; } || \
	{ [ -n "$B_CSID" ] && [ "$B_CSID" != "-" ]; }
}

echo ""
echo "=========================================="
if [ "$NEGCTL" = "1" ]; then
	if anyone_claimed_membership; then
		echo "  NEGATIVE CONTROL FAILED: a node asserted membership or held"
		echo "  a cluster system id with NO node holding quorum by its own"
		echo "  votes. Nothing granted either -- so it was fabricated, and"
		echo "  the proof run's own MEMBER/CSID cannot be believed."
		echo "=========================================="
		exit 1
	fi
	echo "  NEGATIVE CONTROL HELD: with VOTES=0 on both nodes, neither"
	echo "  founded, neither reached MEMBER and neither holds a CSID."
	echo "  The founder + CSID the proof run reports are therefore"
	echo "  measurements of real executive state, not constants."
	echo "=========================================="
	exit 0
fi

if cn2_reached && [ "$A_ROLE" = "founder" ] && [ "$B_ROLE" = "joiner" ]; then
	echo "  GENESIS 2-NODE PROOF PASSED"
	echo "  Node A founded a VMScluster and node B joined it: both"
	echo "  executives report VMS_CLUSTER_MEMBER with CN=2."
	echo "=========================================="
	exit 0
fi

echo "  GENESIS 2-NODE PROOF FAILED"
echo "  A: role=${A_ROLE:-?} member=${A_MEMBER:-?} cn=${A_CN:-?}"
echo "  B: role=${B_ROLE:-?} member=${B_MEMBER:-?} cn=${B_CN:-?}"
if [ "$A_ROLE" = "founder" ] && [ "$A_MEMBER" = "1" ]; then
	echo "  (GENESIS itself HELD: node A founded with CSID ${A_CSID} and"
	echo "   its executive reports MEMBER. What did not happen is node B's"
	echo "   ADMISSION -- read RIG-B-JOINREC and the %CNXMAN transcript.)"
fi
echo "=========================================="
echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null
echo "--- node B console tail ---"; tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null
exit 1
