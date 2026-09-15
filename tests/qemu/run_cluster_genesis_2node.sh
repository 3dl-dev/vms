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
# THREE MODES (RIG_MODE), and the two controls are what give the proof teeth:
#
#   proof   (default) A founds, B joins, both reach MEMBER with CN=2.
#
#   negctl  node A is given VOTES=0 too. Nothing else changes. Neither node
#           then satisfies quorum by its own votes, so neither may found, so no
#           cluster exists to join: no node may reach MEMBER or hold a CSID.
#           This is what distinguishes a measured genesis from a printed one.
#
#   ambig   node B is given SCSSYSTEMID 1030 instead of 1026. Nothing else
#           changes. 1030 & 0x3ff = 6, while the CSV slot the coordinator would
#           assign is 2 -- the two candidate CSID-assignment rules (rd vms-3a7c)
#           DISAGREE about what to call this system, so the coordinator refuses
#           the admission and emits NO op-0x06 membership record.
#           This is the INV-6 control on the JOINER: with no real op-0x06 to
#           learn a generation from, node B must stay NEW with no CSID and must
#           NOT reach MEMBER. It is the same code and the same wire as the proof
#           run, one SYSGEN digit apart -- so a MEMBER in the proof run can only
#           have come from a real, wire-learned CSID.
#
#   rejoin  (rd vms-4838, REJOIN-AS-TARGET) node B joins normally, is then
#           EVACUATED (its own QEMU process killed -9, no graceful shutdown)
#           while node A still holds its CSB inside RECNXINTERVAL, and is
#           relaunched with the SAME SYSGEN identity -- a real reboot, not a
#           network blackout. join_cm_take_held() (vms_cnxman_join_fsm.c)
#           must have node B adopt node A's member-initiated VMS$VAXcluster
#           connection instead of opening a second one: node B's OWN passive
#           capture of its rejoin boot must show ZERO VMS$VAXcluster
#           CONNECT_REQ frames sourced from its own MAC, and both nodes must
#           end at MEMBER, CN=2. Node B's two boots go through the
#           reconnect-tolerant segment relay (segment_relay.py --reconnect-b),
#           since node A's own netdev is never restarted.
#
#   xnode   (rd vms-94c) the proof run, PLUS a cross-node DLM phase after
#           membership settles. Each node scans its OWN candidate name set,
#           finds one the PEER masters (read back from GET_RESMASTER, never
#           computed), takes an EX lock on it across the wire, contends with a
#           second incompatible request so the master owes a BLOCKING AST, and
#           then releases the holder so a $DEQ crosses. What that makes happen
#           on the wire is an op-0x03 and an op-0x04 IN EACH DIRECTION -- the
#           two frames the arm's emit half (rd vms-d7a3) builds and which no
#           2-node run had yet entered.
#
#           IT IS ALSO THE RECEIVE PROOF AND THE NEVER-CRASH-A-PEER PROOF.
#           Since rd vms-c72 the arm CONSUMES both opcodes, so each peer's
#           frames must show up as ACTIONS on the receiving node's own lock
#           database -- releases_received (a $DEQ that really released an LKB)
#           and blkasts_received -- and BOTH nodes must still report member=1
#           cn=2 with their two membership projections agreeing and neither
#           console panicked. Acting on a peer's frame is a strictly harder
#           survival claim than declining it.
#
#   remaster (rd vms-1ee, DLM rung H10a re-established, executive-resident)
#           the proof run, PLUS node B's process simply EXITS on its own after
#           its (shorter) window -- no last-gasp announcement, no kill -9.
#           Node A, left running, discovers (before B leaves) a name B
#           genuinely masters, then relies on nothing but its own
#           connectivity-loss ladder to notice the departure: RECNXINTERVAL's
#           reconnect hold expires unanswered, the coordinator's transition
#           genuinely REMOVES node B's CSB, and that removal fires the DLM
#           arm's member_departed callback as a DIRECT CALL from
#           cnxman_notify_membership_changes() (vms_cnxman.c) -- never an
#           ioctl issued on the departed peer's behalf. Node A then re-$ENQs
#           the SAME name and reads GET_RESMASTER back to prove it now
#           masters it locally: the autonomous remaster, unassisted.
#
# VERDICT: read only from the guests' own RIG-* lines, which carry values the
# guest read back out of the executive (INV-6).

set -uo pipefail

OUT="${OUT_DIR:-/out}"
mkdir -p "$OUT"

MODE="${RIG_MODE:-proof}"
[ "${RIG_NEGCTL:-0}" = "1" ] && MODE=negctl   # back-compat with the first rig
NEGCTL=0
[ "$MODE" = "negctl" ] && NEGCTL=1
GROUP="${RIG_GROUP:-2026}"
RECNX="${RIG_RECNX:-8}"           # RECNXINTERVAL == the genesis discovery window
# CLUSTER_CREDITS: the receive-buffer grant each port advertises at abs 95 of
# its formation body. Load-bearing: with 0 the circuit opens and the peer may
# put NOTHING on it (p. 2-43 "no credit, no message"), which is exactly what a
# first run of this rig measured -- both VCs OPEN, credits_send=0, and not one
# sequenced frame in either direction. 32 is VMS's own default.
CREDITS="${RIG_CREDITS:-32}"
# This build's own advertised software token (spec SS4(g)); the split-brain
# gate (rd vms-1ee) proves two nodes are the same implementation by it.
SWVER="${RIG_SWVER:-OVMX0.6}"
STAGGER="${RIG_STAGGER:-30}"      # seconds between powering on A and B
WINDOW_A="${RIG_WINDOW_A:-150}"   # how long each node polls the executive
WINDOW_B="${RIG_WINDOW_B:-110}"
WALL="${RIG_WALL:-600}"

# rd vms-94c. The cross-node phase runs AFTER each node's membership window, so
# the two windows must END together or one node would do its cross-node work
# against a peer that has already powered off. B is started STAGGER seconds
# late, so its window is that much shorter and both phases begin at the same
# wall moment. LINGER then keeps each node up past its OWN phase, which is what
# makes the survival verdict a statement about the PEER's frames.
XNODE=0
LINGER="${RIG_LINGER:-45}"
# rd vms-b6d (FC-P8.1): the QUORUM-HANG run. See the block comment above
# qhang_schedule() below for the timing and what each number is chosen against.
QHANG=0
CUT_AT=0
HEAL_AT=0
if [ "$MODE" = "qhang" ]; then
	QHANG=1
	#
	# THE SCHEDULE, AND WHAT EACH NUMBER IS CHOSEN AGAINST. Three of the
	# executive's own timers decide it, and getting any of them backwards
	# produces a run that measures nothing:
	#
	#   RECNXINTERVAL (45s here) is BOTH the genesis discovery window AND
	#     p. 7-30's reconnect hold. As the discovery window it means node A
	#     founds 45s after ITS cluster starts -- and genesis is refused while
	#     any peer is audible, so THE STAGGER MUST EXCEED IT or node B
	#     arrives first and neither node ever founds (measured: a 30s stagger
	#     against a 45s window left both nodes JOINING forever). As the
	#     reconnect hold it is the budget the heal has to land inside, or the
	#     peer is removed from the membership instead of reconnected.
	#   The channel listen timeout (20s, vms_pe_fsm.h) is how long after the
	#     cut a node's own executive takes to NOTICE it. So the cut must be
	#     at least that long before anything can be observed, and the heal
	#     must come after the observation but inside the reconnect hold.
	#   The windows are sized so both nodes ENTER the quorum-hang phase
	#     before the cut: A at WINDOW_A after its own boot, B at
	#     WINDOW_A - STAGGER after its later one, which is the same wall
	#     moment.
	#
	RECNX="${RIG_RECNX:-45}"
	STAGGER="${RIG_STAGGER:-55}"     # > RECNX, or nobody founds
	WINDOW_A="${RIG_WINDOW_A:-85}"
	WINDOW_B="${RIG_WINDOW_B:-$((WINDOW_A - STAGGER))}"
	WALL="${RIG_WALL:-900}"
	CUT_AT="${RIG_CUT_AT:-100}"      # both phases have begun by now
	HEAL_AT="${RIG_HEAL_AT:-145}"    # 20s to notice + margin, inside the hold
fi
# rd vms-4838 (REJOIN-AS-TARGET): the EVACUATE->REJOIN run. Node B's own QEMU
# process is killed and relaunched with the SAME SYSGEN identity, so this
# reuses qhang's proven RECNX/STAGGER pair (RECNXINTERVAL must survive the
# whole evacuation dwell, and the stagger must exceed it or node A never
# founds) but drives an actual process kill + relaunch instead of a segment
# cut.
REJOIN=0
MEMBER_SEEN=0
if [ "$MODE" = "rejoin" ]; then
	REJOIN=1
	RECNX="${RIG_RECNX:-45}"
	STAGGER="${RIG_STAGGER:-55}"        # > RECNX, or node A never founds
	WINDOW_A="${RIG_WINDOW_A:-200}"     # must outlive both of node B's boots
	B_WINDOW1="${RIG_B_WINDOW1:-45}"    # node B's FIRST boot: only needs to
					     # reach MEMBER before this rig kills it
	B_WINDOW2="${RIG_B_WINDOW2:-60}"    # node B's REJOIN boot: long enough to
					     # settle and dump its own diagnostics
	WAIT_MEMBER_TIMEOUT="${RIG_WAIT_MEMBER_TIMEOUT:-40}"
	POST_MEMBER_SETTLE="${RIG_POST_MEMBER_SETTLE:-3}"
	# > the 20s channel-listen timeout (so node A's executive really NOTICES
	# the departure before the heal), < RECNX (so its CSB is still held).
	EVAC_DWELL="${RIG_EVAC_DWELL:-25}"
	WALL="${RIG_WALL:-420}"
fi
if [ "$MODE" = "xnode" ]; then
	XNODE=1
	WINDOW_A="${RIG_WINDOW_A:-150}"
	WINDOW_B="${RIG_WINDOW_B:-$((WINDOW_A - STAGGER))}"
	WALL="${RIG_WALL:-900}"
fi
# rd vms-1ee (DLM rung H10a re-established, executive-resident). Node B joins
# normally and then simply exits -- its own cluster_node poll loop ends and it
# powers off, a REAL departure with no last-gasp announcement and no kill -9 --
# and node A, left running, is given nothing but its own connectivity-loss
# ladder to notice with: RECNXINTERVAL's reconnect hold expires with nobody
# answering, the coordinator's transition genuinely REMOVES node B's CSB from
# membership, and cnxman_notify_membership_changes() (vms_cnxman.c) fires the
# DLM arm's member_departed callback AS A DIRECT CALL. Node A's own remaster
# phase (cluster_node.c section 6f) discovers a name B masters BEFORE this, then
# polls its own CLUB for cluster_nodes==1, then re-$ENQs the SAME name and reads
# GET_RESMASTER back to prove it now masters it locally.
#
# REMASTER_A/REMASTER_B are per-node (node_cmdline below selects by tag): only
# the SURVIVOR (A) runs the phase -- the node that is itself departing has
# nothing to observe.
REMASTER=0
REMASTER_A=0
REMASTER_B=0
if [ "$MODE" = "remaster" ]; then
	REMASTER=1
	REMASTER_A=1
	# B must be alive when A's OWN settle window ends (so rig_rm_find's
	# discovery finds it still up), then exit well before A's own
	# departure-wait budget (RIG_RM_DEPART_WAIT_MAX = 180s, cluster_node.c)
	# is spent -- comfortable margin on a real process exit, not a kill.
	WINDOW_A="${RIG_WINDOW_A:-40}"
	WINDOW_B="${RIG_WINDOW_B:-70}"
	WALL="${RIG_WALL:-420}"
fi

VOTES_A=1
[ "$MODE" = "negctl" ] && VOTES_A=0

# Node B's SCSSYSTEMID. 1026 & 0x3ff == 2 == the CSV slot the coordinator
# assigns it, so the two candidate CSID rules agree and the admission is
# unambiguous. The `ambig` mode moves it to 1030 (& 0x3ff == 6) so they do not.
SYSID_B=1026
[ "$MODE" = "noderive" ] && SYSID_B=1030

KERNEL=/boot/vmlinuz
INITRD=/initramfs.cpio.gz
SEGMENT_PORT=16009   # the loopback TCP port carrying the L2 segment
RELAY_PORT=16010     # qhang mode: node B dials THIS, and the relay dials A

QEMU=qemu-system-x86_64
if [ -w /dev/kvm ]; then ACCEL="-accel kvm -cpu host"; else ACCEL="-accel tcg"; fi

echo "=== OVMX 2-node cluster GENESIS rig (rd vms-f6b) ==="
echo "mode=$MODE"
echo "accel=${ACCEL#-accel } group=$GROUP recnx=${RECNX}s stagger=${STAGGER}s"
[ "$XNODE" = "1" ] && echo "cross-node phase: ON (windows A=${WINDOW_A}s B=${WINDOW_B}s, linger=${LINGER}s)"
[ "$REJOIN" = "1" ] && echo "rejoin phase: ON (B_WINDOW1=${B_WINDOW1}s evac_dwell=${EVAC_DWELL}s B_WINDOW2=${B_WINDOW2}s)"
[ "$REMASTER" = "1" ] && echo "remaster phase: ON (windows A=${WINDOW_A}s B=${WINDOW_B}s -- B exits on its own, A watches)"
echo "node A: OVMXA/1025 VOTES=$VOTES_A EXPECTED_VOTES=1 VAXCLUSTER=2"
echo "node B: OVMXB/$SYSID_B VOTES=0          VAXCLUSTER=2"
echo ""

# --------------------------------------------------------------------------
# Launching one guest
# --------------------------------------------------------------------------

# The SYSGEN identity rides the kernel command line; the initramfs is symmetric.
node_cmdline() {
	# $1=tag $2=scsnode $3=sysid $4=votes $5=expected_votes $6=window
	local rm=0
	[ "$1" = "A" ] && rm="$REMASTER_A"
	[ "$1" = "B" ] && rm="$REMASTER_B"
	echo "console=ttyS0 net.ifnames=0 biosdevname=0 panic=-1 loglevel=7" \
	     "ovmx.tag=$1 ovmx.scsnode=$2 ovmx.sysid=$3 ovmx.votes=$4" \
	     "ovmx.expected_votes=$5 ovmx.vaxcluster=2 ovmx.group=$GROUP" \
	     "ovmx.recnx=$RECNX ovmx.credits=$CREDITS ovmx.swver=$SWVER" \
	     "ovmx.window=$6 ovmx.xnode=$XNODE ovmx.linger=$LINGER" \
	     "ovmx.qhang=$QHANG ovmx.remaster=$rm"
}

# Node A holds the segment open; node B dials in. A is powered on first
# anyway, so the listener is always up before the connector.
segment_netdev() {
	# $1 = node tag
	if [ "$1" = "A" ]; then
		echo "socket,id=net0,listen=127.0.0.1:${SEGMENT_PORT}"
	elif [ "$QHANG" = "1" ] || [ "$REJOIN" = "1" ]; then
		# THE PARTITION (rd vms-b6d) or THE RECONNECT-TOLERANT LEG
		# (rd vms-4838). B's frames go to segment_relay.py, which
		# forwards them to A -- either dropping them during a cut
		# window (qhang), or, in --reconnect-b mode (rejoin), tolerating
		# node B's own QEMU process being killed and a fresh one
		# reconnecting here while node A's own leg stays up throughout.
		# Neither guest is told anything either way; each discovers
		# what happened the way a real system does.
		echo "socket,id=net0,connect=127.0.0.1:${RELAY_PORT}"
	else
		echo "socket,id=net0,connect=127.0.0.1:${SEGMENT_PORT}"
	fi
}

LAUNCH_PID=0
launch_node() {
	# $1=tag $2=scsnode $3=sysid $4=votes $5=expected_votes $6=mac $7=window
	# $8=outbase (optional; defaults to node<tag>) -- lets RIG_MODE=rejoin
	# keep node B's two boots in their own file sets instead of the second
	# one clobbering the first.
	local tag="$1" mac="$6"
	local outbase="${8:-node${tag}}"
	local append; append=$(node_cmdline "$1" "$2" "$3" "$4" "$5" "$7")
	local netdev; netdev=$(segment_netdev "$tag")

	$QEMU $ACCEL \
		-kernel "$KERNEL" -initrd "$INITRD" \
		-append "$append" \
		-m 512M -smp 1 -nographic -no-reboot -nodefaults \
		-netdev "$netdev" \
		-device "virtio-net-pci,netdev=net0,mac=${mac},romfile=" \
		-serial "file:$OUT/${outbase}.console.log" \
		-serial "file:$OUT/${outbase}.ttyS1.log" \
		-serial "file:$OUT/${outbase}.pcap.b64" \
		>/dev/null 2>&1 &
	LAUNCH_PID=$!
}

echo "--- powering on node A (it must hear nobody for ${RECNX}s, then found) ---"
launch_node A OVMXA 1025 "$VOTES_A" 1 52:54:00:00:10:25 "$WINDOW_A"; PA=$LAUNCH_PID

# THE SEGMENT, WITH A CUT IN IT (rd vms-b6d). Started between the two power-ons
# so its clock and node A's are within a second of each other: the cut must land
# AFTER both nodes have finished their membership window and entered the
# quorum-hang phase, and the heal must land while the peer's CSB is still inside
# its RECNXINTERVAL reconnect hold (p. 7-30) -- which is why RECNX is 45s in this
# mode and the cut is ~45s long.
RELAY_PID=0
if [ "$QHANG" = "1" ]; then
	echo "--- starting the segment relay (cut at t=${CUT_AT}s, heal at t=${HEAL_AT}s) ---"
	python3 /segment_relay.py --listen-port "$RELAY_PORT" \
		--connect-port "$SEGMENT_PORT" \
		--cut-at "$CUT_AT" --heal-at "$HEAL_AT" \
		> "$OUT/relay.log" 2>&1 &
	RELAY_PID=$!
fi
if [ "$REJOIN" = "1" ]; then
	echo "--- starting the segment relay (reconnect-tolerant: node B may evacuate and rejoin) ---"
	python3 /segment_relay.py --listen-port "$RELAY_PORT" \
		--connect-port "$SEGMENT_PORT" --reconnect-b \
		> "$OUT/relay.log" 2>&1 &
	RELAY_PID=$!
fi
sleep "$STAGGER"

if [ "$REJOIN" = "1" ]; then
	echo "--- powering on node B, round 1 (first join) ---"
	launch_node B OVMXB "$SYSID_B" 0 1 52:54:00:00:10:26 "$B_WINDOW1" nodeB-r1
	PB1=$LAUNCH_PID

	echo "--- waiting up to ${WAIT_MEMBER_TIMEOUT}s for node B to reach MEMBER ---"
	SECS=0
	while [ "$SECS" -lt "$WAIT_MEMBER_TIMEOUT" ]; do
		if grep -aq "RIG-B-CLUB.*state=MEMBER" "$OUT/nodeB-r1.ttyS1.log" 2>/dev/null; then
			MEMBER_SEEN=1
			break
		fi
		sleep 1
		SECS=$((SECS + 1))
	done
	if [ "$MEMBER_SEEN" = "1" ]; then
		echo "    node B reached MEMBER at ~t=${SECS}s of its own boot; settling ${POST_MEMBER_SETTLE}s"
		sleep "$POST_MEMBER_SETTLE"
	else
		echo "    node B never reached MEMBER within ${WAIT_MEMBER_TIMEOUT}s -- evacuating anyway"
	fi

	echo "--- EVACUATING node B (kill -9; node A holds its CSB and keeps dialing for ${EVAC_DWELL}s) ---"
	kill -9 "$PB1" 2>/dev/null
	wait "$PB1" 2>/dev/null
	sleep "$EVAC_DWELL"

	echo "--- powering on node B, round 2 (REJOIN, same SYSGEN identity) ---"
	launch_node B OVMXB "$SYSID_B" 0 1 52:54:00:00:10:26 "$B_WINDOW2" nodeB-r2
	PB=$LAUNCH_PID
else
	echo "--- powering on node B (it must join what A formed) ---"
	launch_node B OVMXB "$SYSID_B" 0 1 52:54:00:00:10:26 "$WINDOW_B"
	PB=$LAUNCH_PID
fi

( sleep "$WALL"; kill -9 "$PA" "$PB" 2>/dev/null ) & GUARD=$!
wait "$PA" 2>/dev/null
wait "$PB" 2>/dev/null
kill "$GUARD" 2>/dev/null
[ "$RELAY_PID" != "0" ] && kill "$RELAY_PID" 2>/dev/null

# RIG_MODE=rejoin: the generic helpers below read $OUT/node<tag>.*; point node
# B's at its REJOIN round (r2) -- the state under test -- while r1's files
# stay on disk under their own names for anyone reading the first-join
# baseline.
if [ "$REJOIN" = "1" ]; then
	for ext in console.log ttyS1.log pcap.b64; do
		[ -f "$OUT/nodeB-r2.$ext" ] && cp "$OUT/nodeB-r2.$ext" "$OUT/nodeB.$ext"
	done
fi

# --------------------------------------------------------------------------
# Reading the guests' executive readback
# --------------------------------------------------------------------------

if [ "$QHANG" = "1" ] && [ -s "$OUT/relay.log" ]; then
	echo ""
	echo "=== the segment (rig-side fact: when the wire carried, and when it did not) ==="
	cat "$OUT/relay.log"
fi

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

# THE WIRE HALF of the cross-node evidence (rd vms-94c). Decodes the cat-0x02
# opcode of every 0x6007 frame each node's PASSIVE probe captured, using this
# codebase's own published offsets (tests/qemu/scan_dlm_wire.py). It says a byte
# reached the segment and nothing more; which executive emitted it is the
# RIG-*-DLM-EMIT counters' question, and both are printed.
WIRE_SCAN=""
if command -v python3 >/dev/null 2>&1 && [ -r /scan_dlm_wire.py ]; then
	echo ""
	echo "=== cat-0x02 opcodes ON THE WIRE (each node's own passive capture) ==="
	for N in A B; do
		[ -s "$OUT/node${N}.pcap" ] || continue
		L=$(python3 /scan_dlm_wire.py "$OUT/node${N}.pcap" 2>&1) || true
		echo "$L"
		WIRE_SCAN="$WIRE_SCAN $L"
	done
fi

# How many frames of one opcode the captures showed, summed over both nodes'
# probes. A frame that really crossed the wire is delivered to the RECEIVING
# node's own probe as an ordinary incoming frame regardless of anything else,
# so summing both nodes' pcaps always counts it at least once -- which is why
# this is a >0 gate and never a count the verdict quotes as "how many were
# sent". That number is the arm's.
# (Since rd vms-175's ETH_P_ALL transmit-capture fix, the SENDING node's own
# probe sees the SAME frame too, tagged dir=TX in sca_l2probe's own log --
# before that fix a protocol-specific AF_PACKET bind only ever reached the
# kernel's RECEIVE fan-out, so a sender's own probe could NOT see its own
# transmit. This gate never depended on that -- it only needed the receiving
# side's ordinary incoming capture -- so it was not itself vacuous; it is
# corrected here only because the OLD comment claimed self-TX visibility
# that did not exist yet.)
wire_saw() {
	echo "$WIRE_SCAN" | tr ' ' '\n' | sed -n "s/^$1=//p" \
		| awk '{s+=$1} END {print s+0}'
}

# `RIG-<tag>-FINAL <key>=<value>`: pull one key out of one node's verdict line.
#
# THE `tr -d '\r'` IS NOT COSMETIC. The guest writes these lines to a serial
# TTY, whose line discipline translates \n to \r\n -- so the LAST field on every
# line arrives here with a carriage return glued to its value. A comparison
# against "agree" then fails on a node that really did report `agree`, and the
# run reports a disagreement it never measured. It cost one full rig run
# (rd vms-94c) to find, and every extractor below strips it for that reason.
final_field() {
	# $1=tag $2=key
	grep -a "RIG-$1-FINAL" "$OUT/node$1.ttyS1.log" 2>/dev/null | tail -n 1 \
		| tr -d '\r' | tr ' ' '\n' | sed -n "s/^$2=//p" | tail -n 1
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

# --------------------------------------------------------------------------
# rd vms-94c: pulling the cross-node facts out of the guests' own output
#
# Every one of these reads a line the GUEST printed from a value it had just
# read back out of its executive. Nothing here is computed from the rig's
# configuration, and nothing is inferred from one node's line about the other.
# --------------------------------------------------------------------------

# One `key=value` off a named RIG line. $1=tag $2=line-marker $3=key
# $4=which occurrence (head/tail). An absent line yields the empty string,
# which every caller below renders as "?" rather than as a zero.
rig_field() {
	grep -a "RIG-$1-$2 " "$OUT/node$1.ttyS1.log" 2>/dev/null | "${4:-tail}" -n 1 \
		| tr -d '\r' | tr ' ' '\n' | sed -n "s/^$3=//p" | tail -n 1
}

# One `key=value` off a DLM ledger line taken at a named phase.
# $1=tag $2=line-suffix (EMIT|LEG|POST) $3=phase $4=key
dlm_field() {
	grep -a "RIG-$1-DLM-$2 at=$3 " "$OUT/node$1.ttyS1.log" 2>/dev/null \
		| tail -n 1 | tr -d '\r' | tr ' ' '\n' | sed -n "s/^$4=//p" | tail -n 1
}

# A number the guest printed, or 0 when the line is absent. Used ONLY for
# comparisons that are already reported verbatim beside them.
num() { case "$1" in ''|*[!0-9]*) echo 0 ;; *) echo "$1" ;; esac; }

# Did this node's console take a bugcheck? The strings are the executive's own
# and the kernel's own; a clean run has none of them. This is the half of the
# never-crash-a-peer proof that a missing line could otherwise hide.
console_panicked() {
	grep -aqE 'Kernel panic|BUG: |Oops: |general protection|%CNXMAN, bugcheck|CLUEXIT' \
		"$OUT/node$1.console.log" 2>/dev/null
}

echo ""
echo "=========================================="
if [ "$MODE" = "negctl" ]; then
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

if [ "$MODE" = "noderive" ]; then
	# A must found; B must be ADMITTED and must hold the ASSIGNED slot 2 --
	# which 1030 & 0x3ff = 6 could never have produced.
	if [ "$A_ROLE" != "founder" ] || [ "$A_MEMBER" != "1" ]; then
		echo "  NO-DERIVE CONTROL INCONCLUSIVE: node A did not found."
		echo "=========================================="
		exit 1
	fi
	if [ "$B_MEMBER" != "1" ] || [ "$B_CSID" != "0x00010002" ]; then
		echo "  NO-DERIVE CONTROL FAILED: node B (SCSSYSTEMID 1030) reports"
		echo "  member=$B_MEMBER csid=$B_CSID. It must hold 0x00010002 -- the"
		echo "  CSV slot the coordinator ASSIGNED it. Anything else means the"
		echo "  identity was computed locally rather than adopted off the"
		echo "  wire (1030 & 0x3ff = 6 would give 0x00010006)."
		echo "=========================================="
		exit 1
	fi
	echo "  NO-DERIVE CONTROL HELD (rd vms-3a7c): node B's SCSSYSTEMID is"
	echo "  1030, whose low ten bits are 6 -- and its executive reports CSID"
	echo "  $B_CSID, CSV slot 2, the slot the coordinator assigned. A value"
	echo "  it could not have computed: it was ADOPTED from the op-0x05"
	echo "  membership record, which is the rule the oracle settled."
	echo "=========================================="
	exit 0
fi

if [ "$MODE" = "qhang" ]; then
	# ---------------------------------------------------------------
	# rd vms-b6d (FC-P8.1): THE QUORUM HANG, and its control, in one run.
	#
	# Every value below is a field the GUEST printed from a read of its own
	# executive: the CLUB's quorum_lost, and $GETLKI's granted_mode for a
	# lock the executive really holds. The rig computes none of them, and
	# the only thing it contributes is relay.log's account of when the wire
	# stopped carrying -- which is printed above, separately, because it is
	# a fact about the segment and not about any executive.
	# ---------------------------------------------------------------
	qh_field() {   # $1=tag $2=line-suffix $3=at-phase $4=key
		grep -a "RIG-$1-QH-$2 at=$3 " "$OUT/node$1.ttyS1.log" 2>/dev/null \
			| tail -n 1 | tr -d '\r' | tr ' ' '\n' \
			| sed -n "s/^$4=//p" | tail -n 1
	}
	qh_line() {    # $1=tag $2=line-suffix $3=key
		grep -a "RIG-$1-QH-$2 " "$OUT/node$1.ttyS1.log" 2>/dev/null \
			| tail -n 1 | tr -d '\r' | tr ' ' '\n' \
			| sed -n "s/^$3=//p" | tail -n 1
	}

	A_QLOST=$(qh_field A CLUB during qlost);  B_QLOST=$(qh_field B CLUB during qlost)
	A_QUOR=$(qh_field A CLUB during quorum);  B_QUOR=$(qh_field B CLUB during quorum)
	A_BEF=$(qh_field A ENQ before granted_mode)
	B_BEF=$(qh_field B ENQ before granted_mode)
	A_DUR=$(qh_field A ENQ during granted_mode)
	B_DUR=$(qh_field B ENQ during granted_mode)
	A_DURST=$(qh_field A ENQ during status)
	B_DURST=$(qh_field B ENQ during status)
	B_DEQ=$(qh_field B DEQ during status)
	B_LOSS=$(qh_line B LOSS observed)
	B_BACK=$(qh_line B REGAIN observed)
	B_AFT=$(qh_line B AFTER granted_mode)
	B_RES=$(qh_line B AFTER res)

	echo "  QUORUM-HANG RUN (rd vms-b6d) -- granted_mode 5 = EX granted,"
	echo "  0 = the request exists and is NOT granted (the stall):"
	printf "    A (VOTES=1, keeps quorum): during qlost=%s quorum=%s  \$ENQ before=%s during=%s (status %s)\n" \
		"${A_QLOST:-?}" "${A_QUOR:-?}" "${A_BEF:-?}" "${A_DUR:-?}" "${A_DURST:-?}"
	printf "    B (VOTES=0, loses it):     during qlost=%s quorum=%s  \$ENQ before=%s during=%s (status %s)\n" \
		"${B_QLOST:-?}" "${B_QUOR:-?}" "${B_BEF:-?}" "${B_DUR:-?}" "${B_DURST:-?}"
	printf "    B: loss observed=%s  release during the hang=%s  regain observed=%s  after=%s on %s\n" \
		"${B_LOSS:-?}" "${B_DEQ:-?}" "${B_BACK:-?}" "${B_AFT:-?}" "${B_RES:-?}"
	echo ""

	QFAIL=0
	# (0) The rig must have produced the condition at all. A run where B
	#     never lost quorum measures nothing -- and says so rather than
	#     passing on an assertion it never tested.
	if [ "$B_LOSS" != "1" ] || [ "$B_QLOST" != "1" ]; then
		echo "  INCONCLUSIVE (0): node B never observed a quorum loss."
		echo "  The segment cut did not reach its executive as one, so"
		echo "  nothing below was tested. Read the RIG-B-QH-CLUB lines"
		echo "  and relay.log together."
		QFAIL=1
	fi
	# (1) THE BASELINE. Both nodes must have been granted the same request
	#     while the cluster was whole.
	if [ "$A_BEF" != "5" ] || [ "$B_BEF" != "5" ]; then
		echo "  INCONCLUSIVE (1): a node was not granted its EX lock"
		echo "  BEFORE the cut (A=$A_BEF B=$B_BEF). Without that baseline a"
		echo "  later stall would prove only that this rig cannot lock."
		QFAIL=1
	fi
	# (2) THE STALL: queued, with no error status.
	if [ "$B_DUR" = "5" ]; then
		echo "  FAILED (2): node B GRANTED a clustered \$ENQ while its own"
		echo "  executive reported quorum_lost=1. That is the fabrication"
		echo "  this item exists to remove: a VMScluster that has lost"
		echo "  quorum stalls (p. 7-4), it does not keep granting."
		QFAIL=1
	elif [ "$B_DUR" != "0" ]; then
		echo "  FAILED (2): node B's request during the hang is in"
		echo "  neither state -- granted_mode=$B_DUR."
		QFAIL=1
	fi
	if [ "$B_DURST" != "1" ]; then
		echo "  FAILED (2b): node B's \$ENQ during the hang returned"
		echo "  status=$B_DURST, not SS\$_NORMAL(1). A quorum hang is a"
		echo "  STALL, not an error return: the caller must be left"
		echo "  waiting, never handed a failure VMS does not have."
		QFAIL=1
	fi
	# (3) RELEASES ARE NOT GATED. A node must always be able to give a lock
	#     back while it waits for quorum.
	if [ "$B_DEQ" != "1" ]; then
		echo "  FAILED (3): node B could not \$DEQ a lock it held from"
		echo "  before the loss (status=$B_DEQ). The hang gates GRANTS, and"
		echo "  must leave every release path alone."
		QFAIL=1
	fi
	# (4) THE CONTROL: node A kept quorum on its own vote, so it must NOT
	#     have stalled. This is what separates "stalls on quorum loss" from
	#     "stalls whenever a peer goes away".
	if [ "$A_QLOST" != "0" ]; then
		echo "  INCONCLUSIVE (4): node A also reported quorum_lost=$A_QLOST,"
		echo "  so this run has no un-hung node to control against."
		QFAIL=1
	elif [ "$A_DUR" != "5" ]; then
		echo "  FAILED (4): node A, WHICH STILL HAS QUORUM (its own VOTES=1"
		echo "  meets QUORUM=$A_QUOR), did not get its lock granted"
		echo "  (granted_mode=$A_DUR). The gate is freezing on a departed"
		echo "  peer rather than on a lost quorum."
		QFAIL=1
	fi
	# (5) THE RESUME: the SAME request completes when quorum returns.
	if [ "$B_BACK" != "1" ]; then
		echo "  FAILED (5): node B never saw quorum return after the heal,"
		echo "  so the resume could not be measured. Its stalled request is"
		echo "  still outstanding -- which is the correct behaviour for a"
		echo "  node that still has no quorum, and an untested resume."
		QFAIL=1
	elif [ "$B_AFT" != "5" ]; then
		echo "  FAILED (5): node B regained quorum but its stalled request"
		echo "  was NOT granted (granted_mode=$B_AFT). The stall has no"
		echo "  release path -- which is worse than not stalling."
		QFAIL=1
	fi
	# (6) ...and nobody crashed.
	for N in A B; do
		if console_panicked "$N"; then
			echo "  FAILED (6): node $N's console shows a panic/bugcheck."
			QFAIL=1
		fi
	done

	if [ "$QFAIL" != "0" ]; then
		echo "=========================================="
		exit 1
	fi
	echo "  QUORUM-HANG PROOF PASSED (rd vms-b6d, FC-P8.1):"
	echo "  the segment was cut; node B's executive computed the quorum"
	echo "  loss from its OWN CSB table (qlost=1, quorum=$B_QUOR) and STALLED a"
	echo "  clustered \$ENQ -- SS\$_NORMAL, a real lock id, granted_mode 0 --"
	echo "  while still honouring a \$DEQ; node A, which kept quorum on its"
	echo "  own vote, granted the identical request at once; and when the"
	echo "  segment healed B's SAME stalled request completed at EX."
	echo "=========================================="
	exit 0
fi

if [ "$MODE" = "rejoin" ]; then
	# ---------------------------------------------------------------
	# rd vms-4838 (REJOIN-AS-TARGET): EVACUATE -> REJOIN, live.
	#
	# TWO INDEPENDENT READINGS OF THE SAME FACT, neither inferred from the
	# other:
	#
	#   THE EXECUTIVE'S OWN WORDS. join_cm_take_held() logs EXACTLY ONCE
	#   per join, THROUGH THIS NODE'S OWN ops->log (pr_info on Linux,
	#   reaching the guest's console/dmesg in real time -- not just at
	#   poweroff), the instant cm_connect_suppressed becomes nonzero:
	#   "%CNXMAN, the executive already holds this pair's VMS$VAXcluster
	#   connection: this node opens none of its own...". This IS
	#   cm_connect_suppressed, read straight off the executive (INV-6) --
	#   no new ioctl needed, because the counter's own activation is
	#   already narrated on the channel this rig already captures.
	#
	#   THE WIRE. node B's OWN passive capture of its REJOIN boot,
	#   decoded with this codebase's own published SCS connection-control
	#   layout (scan_connect_wire.py) -- never a guessed offset, never
	#   VMS's own unpublished internals (Rule 8), the SAME classification
	#   the executive's own parser applies (VMS_FCLS_SCS_CONN_CTRL,
	#   content=110, src/kernel-core/vms_cluster_codec.h/.c) narrowed to
	#   ctrl_type==0 (CONNECT_REQ), AND FURTHER NARROWED BY SYSAP NAME (rd
	#   vms-175) -- content=110/ctrl_type=0 alone matches ANY connection's
	#   CONNECT_REQ (SCS$DIRECTORY dials one per lookup), so the count this
	#   gate reads is scan_connect_wire.py's vaxcluster_connect_req_*
	#   fields, which decode the frame's own two 16-byte SYSAP names (abs
	#   76/92) and isolate VMS$VAXcluster specifically -- the ONE
	#   connection join_cm_take_held() is about. The Ethernet source
	#   address is an ordinary 802.3 field, not a VMS wire field, and says
	#   WHO put the frame on the wire.
	#
	# HONEST FINDING FROM THIS RIG'S OWN TIMING (recorded, not smoothed
	# over): node A's redial reliably WINS the race in this topology (A
	# boots first and has been dialling since t=0), so the SAME
	# suppression line also fires on an ORDINARY first join here -- which
	# the fix's own commit says is correct ("either side may open it").
	# That means round 1's own console is NOT usable as a
	# zero-suppression control in THIS rig; it is reported below, honestly,
	# rather than papered over. The claim this run actually proves is the
	# one that matters at R4: the SAME correct mechanism fires across a
	# REAL guest crash and reboot, on the real executive, and the cluster
	# comes back to MEMBER/CN=2 because of it.
	# ---------------------------------------------------------------
	SUPPRESS_MSG="the executive already holds this pair's VMS\$VAXcluster connection"
	echo ""
	echo "=== node B round 1 (first join, before evacuation) -- ttyS1 tail ==="
	tail -n 40 "$OUT/nodeB-r1.ttyS1.log" 2>/dev/null || echo "(no output)"

	R1_SUPPRESSED=0
	grep -aq "$SUPPRESS_MSG" "$OUT/nodeB-r1.console.log" 2>/dev/null && R1_SUPPRESSED=1
	R2_SUPPRESSED=0
	grep -aq "$SUPPRESS_MSG" "$OUT/nodeB-r2.console.log" 2>/dev/null && R2_SUPPRESSED=1

	MAC_A="52:54:00:00:10:25"
	MAC_B="52:54:00:00:10:26"

	CONNECT_SCAN=""
	if command -v python3 >/dev/null 2>&1 && [ -r /scan_connect_wire.py ] \
	   && [ -s "$OUT/nodeB.pcap" ]; then
		CONNECT_SCAN=$(python3 /scan_connect_wire.py \
			--self-mac "$MAC_B" --peer-mac "$MAC_A" \
			"$OUT/nodeB.pcap" 2>&1) || true
		echo ""
		echo "=== node B's REJOIN-round capture: VMS\$VAXcluster CONNECT_REQ census ==="
		echo "$CONNECT_SCAN"
	fi
	# vaxcluster_connect_req_* (not the generic connect_req_*): isolates
	# VMS$VAXcluster's own CONNECT_REQ frames from the SCS$DIRECTORY dials
	# that share the same content=110/ctrl_type=0 shape (rd vms-175).
	B_CONNECT_FROM_SELF=$(num "$(echo "$CONNECT_SCAN" | \
		sed -n 's/.*vaxcluster_connect_req_from_self=\([0-9]*\).*/\1/p' | tail -n1)")
	B_CONNECT_FROM_PEER=$(num "$(echo "$CONNECT_SCAN" | \
		sed -n 's/.*vaxcluster_connect_req_from_peer=\([0-9]*\).*/\1/p' | tail -n1)")

	echo ""
	echo "  REJOIN RUN (rd vms-4838) -- state read back after node B's SECOND"
	echo "  boot, same SYSGEN identity as its first (round 1 member reached: "
	echo "  ${MEMBER_SEEN}):"
	printf "    node A: role=%s member=%s cn=%s csid=%s\n" \
		"${A_ROLE:-?}" "${A_MEMBER:-?}" "${A_CN:-?}" "${A_CSID:-?}"
	printf "    node B: role=%s member=%s cn=%s csid=%s\n" \
		"${B_ROLE:-?}" "${B_MEMBER:-?}" "${B_CN:-?}" "${B_CSID:-?}"
	printf "    node B's own executive: round1 cm_connect_suppressed-fired=%s  round2(REJOIN)=%s\n" \
		"$R1_SUPPRESSED" "$R2_SUPPRESSED"
	printf "    node B's own REJOIN-round capture: VMS\$VAXcluster CONNECT_REQ from itself=%s  from peer=%s\n" \
		"$B_CONNECT_FROM_SELF" "$B_CONNECT_FROM_PEER"

	RFAIL=0
	if [ "$MEMBER_SEEN" != "1" ]; then
		echo "  INCONCLUSIVE (0): node B never reached MEMBER on its FIRST"
		echo "  boot, so there was no real membership to evacuate. Nothing"
		echo "  below was tested."
		RFAIL=1
	fi
	if [ "$R2_SUPPRESSED" != "1" ]; then
		echo "  FAILED (1a): node B's own executive did NOT log"
		echo "  cm_connect_suppressed firing on its REJOIN boot -- see its"
		echo "  round-2 console tail for what it did instead."
		RFAIL=1
	fi
	if [ ! -s "$OUT/nodeB.pcap" ]; then
		echo "  INCONCLUSIVE (1b): node B's REJOIN-round capture is missing or"
		echo "  empty -- the wire half of this proof cannot be read."
		RFAIL=1
	elif [ "$B_CONNECT_FROM_SELF" != "0" ]; then
		echo "  FAILED (1b): node B's own REJOIN-round capture shows"
		echo "  $B_CONNECT_FROM_SELF VMS\$VAXcluster CONNECT_REQ frame(s) FROM"
		echo "  ITS OWN MAC -- it opened a connect of its own instead of"
		echo "  suppressing it, which is the exact fabrication this fix"
		echo "  removes."
		RFAIL=1
	fi
	if ! cn2_reached; then
		echo "  NOT REACHED (2): the two nodes do not both report MEMBER"
		echo "  with CN=2 after node B's rejoin. See RIG-B-JOINREC on the"
		echo "  round-2 transcript above for where the drive stalled -- a"
		echo "  stall AT op-0x02 on the MEMBER-INITIATED connection (i.e."
		echo "  cm_connect_suppressed already fired, from BOTH readings"
		echo "  above) with NO member reciprocation is the KNOWN relocated"
		echo "  frontier rd vms-694 (the member's own recv_seq freeze), NOT"
		echo "  a defect in THIS fix -- report the exact RIG-B-JOINREC/"
		echo "  RIG-B-CDT rows rather than forcing a verdict."
		RFAIL=1
	fi
	for LABEL in A B-r1 B-r2; do
		case "$LABEL" in
			A) F="$OUT/nodeA.console.log" ;;
			*) F="$OUT/node${LABEL}.console.log" ;;
		esac
		if grep -aqE 'Kernel panic|BUG: |Oops: |general protection|%CNXMAN, bugcheck|CLUEXIT' "$F" 2>/dev/null; then
			echo "  FAILED (3): node $LABEL's console shows a panic/bugcheck."
			RFAIL=1
		fi
	done

	echo ""
	echo "  (honest note: round 1 (first join) ALSO shows"
	echo "  cm_connect_suppressed-fired=$R1_SUPPRESSED in this rig's topology --"
	echo "  node A's redial reliably wins the race here, so a plain first join"
	echo "  is not a zero-suppression control on this segment. Per the fix's"
	echo "  own design either side may open the one VMS\$VAXcluster connection;"
	echo "  the property under test is that the SAME mechanism holds across a"
	echo "  REAL crash+reboot, not which side wins a race.)"
	if [ "$RFAIL" = "0" ]; then
		echo "  REJOIN-AS-TARGET PROOF PASSED (rd vms-4838):"
		echo "  node B was evacuated (kill -9, no graceful shutdown) and"
		echo "  relaunched with the SAME SYSGEN identity while node A held"
		echo "  its CSB inside RECNXINTERVAL. On that REJOIN boot, node B's"
		echo "  own executive logged cm_connect_suppressed firing AND its own"
		echo "  passive capture shows ZERO VMS\$VAXcluster CONNECT_REQ frames"
		echo "  sourced from its own MAC -- it did not open a connect of its"
		echo "  own -- and both nodes report MEMBER with CN=2 afterwards."
		echo "  Neither node's console panicked."
		echo "=========================================="
		exit 0
	fi
	echo "=========================================="
	echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null
	echo "--- node B round1 console tail ---"; tail -n 40 "$OUT/nodeB-r1.console.log" 2>/dev/null
	echo "--- node B round2 console tail ---"; tail -n 40 "$OUT/nodeB-r2.console.log" 2>/dev/null
	exit 1
fi

if [ "$MODE" = "xnode" ]; then
	# ---- the six facts, each read off a guest line ----------------------
	A_XRES=$(rig_field A XN-HOLD res);   B_XRES=$(rig_field B XN-HOLD res)
	A_XMAS=$(rig_field A XN-HOLD master_csid)
	B_XMAS=$(rig_field B XN-HOLD master_csid)
	A_REL=$(num "$(dlm_field A EMIT after releases_sent)")
	B_REL=$(num "$(dlm_field B EMIT after releases_sent)")
	A_RNW=$(num "$(dlm_field A EMIT after releases_no_wire_op)")
	B_RNW=$(num "$(dlm_field B EMIT after releases_no_wire_op)")
	A_BLK=$(num "$(dlm_field A EMIT after blkasts_sent)")
	B_BLK=$(num "$(dlm_field B EMIT after blkasts_sent)")
	A_UNP=$(num "$(dlm_field A EMIT survival unparsed)")
	B_UNP=$(num "$(dlm_field B EMIT survival unparsed)")
	# The RECEIVE ledger (rd vms-c72): what the PEER's op-0x03/op-0x04 did to
	# THIS node's lock database. Read off the node's own RIG-*-DLM-RECV /
	# -DLM-EMIT lines, never inferred from the sender's counters.
	A_RRX=$(num "$(dlm_field A RECV survival releases_received)")
	B_RRX=$(num "$(dlm_field B RECV survival releases_received)")
	A_RRF=$(num "$(dlm_field A RECV survival releases_refused)")
	B_RRF=$(num "$(dlm_field B RECV survival releases_refused)")
	A_BRX=$(num "$(dlm_field A EMIT survival blkasts_received)")
	B_BRX=$(num "$(dlm_field B EMIT survival blkasts_received)")
	A_BDL=$(num "$(dlm_field A EMIT survival blkasts_delivered)")
	B_BDL=$(num "$(dlm_field B EMIT survival blkasts_delivered)")
	A_GONE=$(num "$(dlm_field A POST survival lock_gone)")
	B_GONE=$(num "$(dlm_field B POST survival lock_gone)")
	A_PROJ=$(final_field A projections); B_PROJ=$(final_field B projections)
	W_DEQ=$(wire_saw deq); W_BLK=$(wire_saw blkast); W_VBW=$(wire_saw valblk)
	# rd vms-727: the op-0x06 CONVERT-with-VALBLK receive ledger. Read off
	# the node's own RIG-*-DLM-RECV line, right beside releases_received
	# (never inferred from the sender's own emit counters).
	A_LVBST=$(rig_field A LVBWRITE status)
	B_LVBST=$(rig_field B LVBWRITE status)
	A_VBW=$(num "$(dlm_field A RECV survival valblk_writes_received)")
	B_VBW=$(num "$(dlm_field B RECV survival valblk_writes_received)")

	echo "  CROSS-NODE DLM RUN (rd vms-94c) -- every value below was read"
	echo "  back out of the node's own executive, except the two WIRE counts,"
	echo "  which come from the nodes' own passive captures:"
	printf "    A: peer-mastered=%s master_csid=%s releases_sent=%s blkasts_sent=%s unparsed=%s projections=%s\n" \
		"${A_XRES:-none}" "${A_XMAS:-?}" "$A_REL" "$A_BLK" "$A_UNP" "${A_PROJ:-?}"
	printf "    B: peer-mastered=%s master_csid=%s releases_sent=%s blkasts_sent=%s unparsed=%s projections=%s\n" \
		"${B_XRES:-none}" "${B_XMAS:-?}" "$B_REL" "$B_BLK" "$B_UNP" "${B_PROJ:-?}"
	printf "    WIRE: op-0x03 deq frames=%s  op-0x04 blkast frames=%s  op-0x06 valblk frames=%s\n" \
		"$W_DEQ" "$W_BLK" "$W_VBW"
	printf "    RECEIVED: A releases=%s (refused %s) blkasts=%s (delivered %s)\n" \
		"$A_RRX" "$A_RRF" "$A_BRX" "$A_BDL"
	printf "              B releases=%s (refused %s) blkasts=%s (delivered %s)\n" \
		"$B_RRX" "$B_RRF" "$B_BRX" "$B_BDL"
	printf "    LVB WRITE (rd vms-727): A demote-status=%s  B demote-status=%s\n" \
		"${A_LVBST:-?}" "${B_LVBST:-?}"
	printf "              A valblk_writes_received=%s  B valblk_writes_received=%s\n" \
		"$A_VBW" "$B_VBW"
	echo ""

	XFAIL=0
	# (1) The cluster must still be the cluster.
	if ! cn2_reached; then
		echo "  FAILED (1): the two nodes do not both report MEMBER with CN=2"
		echo "  AFTER the cross-node phase -- so nothing measured after it"
		echo "  can be attributed to a working cluster."
		XFAIL=1
	fi
	# (2) A name must be mastered on the PEER, on both nodes, and the peer
	#     it names must be the OTHER node's real CSID. This is the routing
	#     fact; without it no frame that follows is cross-node at all.
	if [ -z "$A_XRES" ] || [ -z "$B_XRES" ]; then
		echo "  FAILED (2): a node found no resource its PEER masters. Rung"
		echo "  A\" did not route a name off-node, so the emit paths were"
		echo "  never entered. Read the RIG-*-XN-LOCAL lines: they carry the"
		echo "  dir_csid the executive resolved for each candidate."
		XFAIL=1
	elif [ "$A_XMAS" != "$B_CSID" ] || [ "$B_XMAS" != "$A_CSID" ]; then
		echo "  FAILED (2): the master CSID a node read back for its"
		echo "  cross-node resource is not the OTHER node's CSID"
		echo "  (A saw $A_XMAS, B holds $B_CSID; B saw $B_XMAS, A holds $A_CSID)."
		XFAIL=1
	fi
	# (3a) The RELEASE, counted by the ARM THAT SENT IT, and seen on the wire.
	if [ "$A_REL" -lt 1 ] || [ "$B_REL" -lt 1 ]; then
		echo "  FAILED (3a): an op-0x03 \$DEQ was not emitted by both arms"
		echo "  (A=$A_REL B=$B_REL)."
		if [ "$A_RNW" = "0" ] && [ "$B_RNW" = "0" ] && \
		   { [ "$A_GONE" -gt 0 ] || [ "$B_GONE" -gt 0 ]; }; then
			echo ""
			echo "  AND THE ARM NEVER GOT AS FAR AS REFUSING: releases_no_wire_op"
			echo "  is 0 on both, so the \$DEQ never reached the requester FSM at"
			echo "  all -- it died one step earlier, at posts_lock_gone"
			echo "  (A=$A_GONE B=$B_GONE). The engine posts a release to the FORK"
			echo "  thread carrying only the lock id, and the fork thread REBUILDS"
			echo "  the request from the lock database (vms_dlm_scs.c"
			echo "  dlm_arm_run_post -> vms_lock_dlm_proxy_refill_post). For a"
			echo "  \$DEQ that rebuild can never succeed: the operation being"
			echo "  transmitted is the one that destroys the proxy LKB it would be"
			echo "  rebuilt from, and vms_deq_core tears that LKB down as soon as"
			echo "  the post is queued. This is a PRODUCT GAP the rig measured,"
			echo "  not a rig failure, and it is reported rather than worked"
			echo "  around."
		fi
		XFAIL=1
	fi
	if [ "$W_DEQ" -lt 1 ]; then
		echo "  FAILED (3a-wire): no op-0x03 frame appears in either node's"
		echo "  capture. The executive counter and the wire agree that none"
		echo "  crossed."
		XFAIL=1
	fi
	# (3b) The BLOCKING AST, same two independent readings.
	if [ "$A_BLK" -lt 1 ] || [ "$B_BLK" -lt 1 ]; then
		echo "  FAILED (3b): an op-0x04 BLKAST was not emitted by both arms"
		echo "  (A=$A_BLK B=$B_BLK). A master owes one only when a remote"
		echo "  request QUEUES behind a lock held for a remote CSID -- check"
		echo "  queued_no_reply and blkasts_no_wire_op on the same line."
		XFAIL=1
	fi
	if [ "$W_BLK" -lt 1 ]; then
		echo "  FAILED (3b-wire): no op-0x04 frame appears in either node's"
		echo "  capture, so the arm's blkasts_sent cannot be corroborated."
		XFAIL=1
	fi
	# (4) THE RECEIVE ASSERTION, AND THE NEVER-CRASH-A-PEER ASSERTION WITH
	#     IT. Until rd vms-c72 the arm had no receive half, so this gate read
	#     the DECLINE (`unparsed` >= 2 per node) -- "the frame arrived, the
	#     executive refused it, and the node lived". The arm now DELIVERS
	#     both opcodes, so the same two frames must show up as ACTIONS on the
	#     receiving node's own lock database instead:
	#       releases_received  a peer's $DEQ really released an LKB here
	#                          (releases_refused is its honest alternative --
	#                          reported, and not accepted as the receive);
	#       blkasts_received   a peer's BLKAST really reached this arm.
	#     `unparsed` staying put is now the tell that nothing was DECLINED.
	if [ "$A_RRX" -lt 1 ] || [ "$B_RRX" -lt 1 ]; then
		echo "  FAILED (4a): a node did not record RELEASING a lock on the"
		echo "  peer's $DEQ (A releases_received=$A_RRX refused=$A_RRF;"
		echo "  B releases_received=$B_RRX refused=$B_RRF). The frame may"
		echo "  have arrived, but this executive's lock database did not"
		echo "  move, which is what the receive half exists to do."
		XFAIL=1
	fi
	if [ "$A_BRX" -lt 1 ] || [ "$B_BRX" -lt 1 ]; then
		echo "  FAILED (4b): a node did not record RECEIVING the peer's"
		echo "  op-0x04 BLKAST (A blkasts_received=$A_BRX delivered=$A_BDL;"
		echo "  B blkasts_received=$B_BRX delivered=$B_BDL)."
		XFAIL=1
	fi
	# (5) ...and survived them, in the executive's own words.
	if [ "$A_PROJ" != "agree" ] || [ "$B_PROJ" != "agree" ]; then
		echo "  FAILED (5): a node's two membership projections DISAGREE"
		echo "  after the frames arrived (A=$A_PROJ B=$B_PROJ)."
		XFAIL=1
	fi
	for N in A B; do
		if console_panicked "$N"; then
			echo "  FAILED (5): node $N's console shows a bugcheck/panic."
			XFAIL=1
		fi
	done
	# (5b) THE VALUE-BLOCK WRITE CROSSING (rd vms-727). Each node demoted its
	#      cross-node EX grant to CR carrying a 16-byte pattern in the value
	#      block, which must emit op-0x06 to the PEER that masters it. Two
	#      independent facts, same shape as (3a)/(4a) above:
	#        ON THE WIRE      an op-0x06 CONVERT-with-VALBLK frame really
	#                         reached the segment (scan_dlm_wire.py, a byte
	#                         reached the wire -- says nothing about receipt);
	#        APPLIED          the MASTER's own valblk_writes_received counter
	#                         rose -- the receive-ledger fact that it is the
	#                         one that really wrote the block into the RSB
	#                         it masters, which no pcap can show.
	#      Never-crash is already covered by (5) above: this assertion only
	#      adds whether the write was APPLIED, not whether the peer survived
	#      receiving it.
	if [ "$A_LVBST" != "1" ] || [ "$B_LVBST" != "1" ]; then
		echo "  FAILED (5b): a node's own demote-from-write CONVERT did not"
		echo "  report SS\$_NORMAL (A status=$A_LVBST B status=$B_LVBST) --"
		echo "  see the RIG-*-LVBWRITE line for whether it even found a"
		echo "  peer-mastered lock to demote."
		XFAIL=1
	fi
	if [ "$W_VBW" -lt 1 ]; then
		echo "  FAILED (5b-wire): no op-0x06 CONVERT-with-VALBLK frame appears"
		echo "  in either node's own passive capture."
		XFAIL=1
	fi
	if [ "$A_VBW" -lt 1 ] || [ "$B_VBW" -lt 1 ]; then
		echo "  FAILED (5b-applied): a node's own diag does not show it"
		echo "  APPLIED a peer's op-0x06 value-block write (A"
		echo "  valblk_writes_received=$A_VBW B valblk_writes_received=$B_VBW)."
		echo "  The frame may have reached the wire, but the receiving"
		echo "  executive's resource block did not move."
		XFAIL=1
	fi

	# (5c) THE VALUE-BLOCK READ CROSSING (rd vms-727, #1190) -- the symmetric
	#      mirror of (5b): a PEER's cross-node $ENQ...LCK$M_VALBLK must come
	#      back carrying the MASTER's own value block, in the grant reply
	#      (the op-0x01 grant-with-valblk record). Neither node is hard-wired
	#      writer or reader -- the resource's own directory hash decides that
	#      at run time (rig_lvbrd_phase), so this reads BOTH nodes' lines and
	#      classifies by what each one actually reported, never by tag.
	#        WRITE-AND-HOLD  exactly one node's RIG-*-LVBRDHOLD reports
	#                        SS$_NORMAL: it mastered the dedicated name and
	#                        wrote+held the pattern for the other to read;
	#        WIRE            an op-0x01 GRANT-with-valblk frame really
	#                        reached the segment (scan_dlm_wire.py's own
	#                        op01valblk counter -- a byte reached the wire,
	#                        says nothing about receipt);
	#        READ-APPLIED    the OTHER node's RIG-*-GETLKI reports matched=1
	#                        with the exact pattern -- read back off ITS OWN
	#                        $GETLKI, not inferred from the writer's side.
	A_RDHOLD_ST=$(rig_field A LVBRDHOLD status)
	B_RDHOLD_ST=$(rig_field B LVBRDHOLD status)
	A_RD_MATCHED=$(rig_field A GETLKI matched)
	B_RD_MATCHED=$(rig_field B GETLKI matched)
	A_RD_ASCII=$(rig_field A GETLKI valblk_ascii)
	B_RD_ASCII=$(rig_field B GETLKI valblk_ascii)
	W_OP01=$(wire_saw op01valblk)
	LVBRD_PATTERN="OVMXLVBREAD00001"

	echo "  LVB READ CROSSING (rd vms-727, #1190):"
	printf "    A: LVBRDHOLD-status=%s GETLKI-matched=%s GETLKI-ascii=%s\n" \
		"${A_RDHOLD_ST:-none}" "${A_RD_MATCHED:-none}" "${A_RD_ASCII:-none}"
	printf "    B: LVBRDHOLD-status=%s GETLKI-matched=%s GETLKI-ascii=%s\n" \
		"${B_RDHOLD_ST:-none}" "${B_RD_MATCHED:-none}" "${B_RD_ASCII:-none}"
	printf "    WIRE: op-0x01 grant-with-valblk frames=%s\n" "$W_OP01"

	LVBRD_WRITER=0
	if [ "$A_RDHOLD_ST" = "1" ] && [ "$B_RDHOLD_ST" != "1" ]; then
		LVBRD_WRITER=1
	elif [ "$B_RDHOLD_ST" = "1" ] && [ "$A_RDHOLD_ST" != "1" ]; then
		LVBRD_WRITER=1
	fi

	LVBRD_READER=0
	if [ "$A_RD_MATCHED" = "1" ] && [ "$A_RD_ASCII" = "$LVBRD_PATTERN" ]; then
		LVBRD_READER=1
	fi
	if [ "$B_RD_MATCHED" = "1" ] && [ "$B_RD_ASCII" = "$LVBRD_PATTERN" ]; then
		LVBRD_READER=1
	fi

	if [ "$LVBRD_WRITER" != "1" ]; then
		echo "  FAILED (5c-write): exactly one node must report"
		echo "  RIG-*-LVBRDHOLD status=1 (it mastered the dedicated name"
		echo "  and wrote+held the pattern) -- see the RIG-*-LVBRD-PEER /"
		echo "  RIG-*-LVBRD lines above for which node found what."
		XFAIL=1
	fi
	if [ "$W_OP01" -lt 1 ]; then
		echo "  FAILED (5c-wire): no op-0x01 GRANT-with-valblk frame"
		echo "  appears in either node's own passive capture."
		XFAIL=1
	fi
	if [ "$LVBRD_READER" != "1" ]; then
		echo "  FAILED (5c-applied): neither node's own RIG-*-GETLKI"
		echo "  reports matched=1 with the exact written pattern"
		echo "  ($LVBRD_PATTERN) -- the peer's cross-node \$ENQ never"
		echo "  read the master's value block back, or read the wrong"
		echo "  one."
		XFAIL=1
	fi

	# (6) THE DIRECT MASTER-SIDE RELEASE PROOF (rd vms-c72, conductor
	#     ledger-bar gap-close). A DEDICATED single-holder resource's
	#     GET_RESMASTER, sampled on the MASTER once BEFORE the release and
	#     again only once its own `releases_received` counter has RISEN --
	#     never on the counter alone (return-value != state trap).
	A_RMB_RES=$(rig_field A RESMASTER-BEFORE res)
	B_RMB_RES=$(rig_field B RESMASTER-BEFORE res)
	A_RMB_NG=$(num "$(rig_field A RESMASTER-BEFORE n_granted)")
	B_RMB_NG=$(num "$(rig_field B RESMASTER-BEFORE n_granted)")
	A_RMA_NG=$(num "$(rig_field A RESMASTER-AFTER n_granted)")
	B_RMA_NG=$(num "$(rig_field B RESMASTER-AFTER n_granted)")
	A_RMA_FOUND=$(num "$(rig_field A RESMASTER-AFTER found)")
	B_RMA_FOUND=$(num "$(rig_field B RESMASTER-AFTER found)")
	A_RMA_ROSE=$(rig_field A RESMASTER-AFTER counter_rose)
	B_RMA_ROSE=$(rig_field B RESMASTER-AFTER counter_rose)

	echo "  DIRECT RELEASE PROOF (rd vms-c72):"
	printf "    A: BEFORE res=%s n_granted=%s  AFTER n_granted=%s found=%s counter_rose=%s\n" \
		"${A_RMB_RES:-none}" "$A_RMB_NG" "$A_RMA_NG" "$A_RMA_FOUND" "${A_RMA_ROSE:-?}"
	printf "    B: BEFORE res=%s n_granted=%s  AFTER n_granted=%s found=%s counter_rose=%s\n" \
		"${B_RMB_RES:-none}" "$B_RMB_NG" "$B_RMA_NG" "$B_RMA_FOUND" "${B_RMA_ROSE:-?}"

	# A node is a SUBJECT once its own BEFORE found a single holder (n_granted
	# read >=1); CLEAN means that holder is later gone AND the release was
	# actually observed (counter_rose=1); a REAL BUG is the counter rising
	# while the holder is still shown present -- reported, never masked.
	DIRECT_SUBJECT=0; DIRECT_PASS=0; DIRECT_BUG=0
	for N in A B; do
		eval "ng=\$${N}_RMB_NG; rose=\$${N}_RMA_ROSE; ang=\$${N}_RMA_NG; afound=\$${N}_RMA_FOUND"
		if [ "$ng" -ge 1 ] 2>/dev/null; then
			DIRECT_SUBJECT=1
			if [ "$rose" = "1" ] && [ "$ang" -ge 1 ] 2>/dev/null; then
				echo "  REAL BUG (6): node $N's master-side queue STILL shows the"
				echo "  holder present (n_granted=$ang) after releases_received"
				echo "  rose -- the return-value != state trap: the release path"
				echo "  reported success but the LKB never left the granted queue."
				DIRECT_BUG=1
			elif [ "$rose" = "1" ] && { [ "$ang" = "0" ] || [ "$afound" = "0" ]; }; then
				DIRECT_PASS=1
			fi
		fi
	done
	if [ "$DIRECT_BUG" = "1" ]; then
		XFAIL=1
	elif [ "$DIRECT_SUBJECT" = "0" ]; then
		echo "  INCONCLUSIVE (6): neither node found a dedicated single-holder"
		echo "  resource its peer mastered -- the direct delta was never"
		echo "  bracketed."
		XFAIL=1
	elif [ "$DIRECT_PASS" = "0" ]; then
		echo "  INCONCLUSIVE (6): a subject was found but releases_received"
		echo "  never rose within the poll window -- the peer's DEQ was never"
		echo "  observed as processed."
		XFAIL=1
	fi

	# ---- what HELD, itemised, pass or fail ------------------------------
	#
	# A red run is not an absence of evidence. The five properties below are
	# independent, and a run that establishes four of them has established
	# four of them -- saying so is the difference between a proof campaign and
	# a pass/fail light. Each line is printed from the same executive-read
	# variables the gates above tested, so it cannot drift from the verdict.
	held() { [ "$1" = "1" ] && echo "  HELD        $2" || echo "  NOT PROVEN  $2"; }
	echo "  --- what this run established ---"
	held "$( { cn2_reached && [ "$A_PROJ" = agree ] && [ "$B_PROJ" = agree ]; } \
		&& echo 1 || echo 0)" \
		"both nodes MEMBER, CN=2, projections agree AFTER the phase"
	held "$( { [ -n "$A_XRES" ] && [ -n "$B_XRES" ] && \
		 [ "$A_XMAS" = "$B_CSID" ] && [ "$B_XMAS" = "$A_CSID" ]; } \
		&& echo 1 || echo 0)" \
		"a lock genuinely CROSSED: each node holds EX on a name its PEER masters"
	held "$( { [ "$A_BLK" -ge 1 ] && [ "$B_BLK" -ge 1 ] && [ "$W_BLK" -ge 1 ]; } \
		&& echo 1 || echo 0)" \
		"op-0x04 BLKAST emitted by both arms AND seen on the wire"
	held "$( { [ "$A_REL" -ge 1 ] && [ "$B_REL" -ge 1 ] && [ "$W_DEQ" -ge 1 ]; } \
		&& echo 1 || echo 0)" \
		"op-0x03 \$DEQ emitted by both arms AND seen on the wire"
	held "$( { [ "$A_RRX" -ge 1 ] && [ "$B_RRX" -ge 1 ] && \
		 [ "$A_BRX" -ge 1 ] && [ "$B_BRX" -ge 1 ]; } \
		&& echo 1 || echo 0)" \
		"the RECEIVE half ran: each node's own lock database moved on the"
	echo "              peer's op-0x03 (\$DEQ released) and op-0x04 (BLKAST taken in)"
	held "$( { ! console_panicked A && ! console_panicked B; } \
		&& echo 1 || echo 0)" \
		"NEVER CRASH A PEER: both nodes ACTED on the peer's cross-node"
	echo "              frames and stayed sane members (no bugcheck, no panic)"
	held "$( { [ "$DIRECT_BUG" = "0" ] && [ "$DIRECT_PASS" = "1" ]; } \
		&& echo 1 || echo 0)" \
		"the DIRECT release delta: a dedicated single-holder resource's"
	echo "              master-side queue shows the holder GONE after"
	echo "              releases_received rose -- not just the counter"
	held "$( { [ "$A_VBW" -ge 1 ] && [ "$B_VBW" -ge 1 ] && [ "$W_VBW" -ge 1 ]; } \
		&& echo 1 || echo 0)" \
		"op-0x06 CONVERT-with-VALBLK (rd vms-727): a demote-from-write really"
	echo "              crossed and the MASTER's own diag shows it APPLIED the"
	echo "              peer's value block, seen on the wire and never a crash"
	held "$( { [ "$LVBRD_WRITER" = "1" ] && [ "$LVBRD_READER" = "1" ] && \
		 [ "$W_OP01" -ge 1 ]; } && echo 1 || echo 0)" \
		"LVB READ crossing (rd vms-727, #1190): the master's write was read"
	echo "              back over a cross-node \$ENQ...LCK\$M_VALBLK, seen on the"
	echo "              wire as an op-0x01 grant-with-valblk and confirmed by the"
	echo "              reader's own \$GETLKI"
	echo ""

	if [ "$XFAIL" = "0" ]; then
		echo "  CROSS-NODE DLM PROOF PASSED (rd vms-94c)"
		echo "  Each node took an EX lock on a resource ITS PEER masters --"
		echo "  read back from the executive as master_csid=<the peer>,"
		echo "  is_local_master=0 -- contended with a second incompatible"
		echo "  request, and released. The arms report the two frames they"
		echo "  really emitted (op-0x03 \$DEQ, op-0x04 BLKAST), each peer"
		echo "  reports having RECEIVED and honestly DECLINED them, and both"
		echo "  nodes are still MEMBERs with CN=2 and agreeing projections"
		echo "  afterwards. NEVER CRASH A PEER: held, against a live peer"
		echo "  executive. Each node also demoted its cross-node EX grant"
		echo "  to CR carrying a value block (rd vms-727): op-0x06 crossed"
		echo "  on the wire AND the master's own diag shows it APPLIED the"
		echo "  peer's write. And on a separate dedicated resource, the"
		echo "  symmetric LVB READ crossing held too (rd vms-727, #1190):"
		echo "  the node the directory made master wrote+held a pattern,"
		echo "  the peer's cross-node \$ENQ...LCK\$M_VALBLK grant carried it"
		echo "  back as an op-0x01 record seen on the wire, and the peer's"
		echo "  own \$GETLKI confirms it read exactly what was written."
		echo "=========================================="
		exit 0
	fi
	echo "=========================================="
	echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null
	echo "--- node B console tail ---"; tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null
	exit 1
fi

# --------------------------------------------------------------------------
# rd vms-1ee (DLM rung H10a re-established): THE AUTONOMOUS REMASTER.
#
# This mode's own verdict, read entirely off node A's RIG-A-REMASTER-* lines
# (cluster_node.c section 6f). It does NOT fall through to the generic
# cn2_reached() check below: by the time this phase completes, node B has
# genuinely departed and node A correctly reports cn=1 -- a cn2_reached()
# failure here would be measuring the wrong thing.
# --------------------------------------------------------------------------
if [ "$MODE" = "remaster" ]; then
	RM_BEFORE_RES=$(rig_field A REMASTER-BEFORE res)
	RM_MC_BEFORE=$(rig_field A REMASTER-BEFORE master_csid)
	RM_DEPARTED=$(rig_field A REMASTER-DEPARTED observed)
	RM_NODES=$(rig_field A REMASTER-DEPARTED nodes)
	RM_ENQ_STATUS=$(rig_field A REMASTER-AFTER enq_status)
	RM_LKID=$(rig_field A REMASTER-AFTER lkid)
	RM_LOCAL=$(rig_field A REMASTER-AFTER is_local_master)
	RM_MC_AFTER=$(rig_field A REMASTER-AFTER master_csid_after)
	RM_REMASTERED=$(rig_field A REMASTER-AFTER remastered)

	echo ""
	echo "=== rd vms-1ee (H10a): the autonomous remaster -- every value below"
	echo "    was read back out of node A's own executive ==="
	printf "    discovered  : res=%s master_csid_before=%s\n" \
		"${RM_BEFORE_RES:-?}" "${RM_MC_BEFORE:-?}"
	printf "    departure   : observed=%s nodes_after=%s\n" \
		"${RM_DEPARTED:-?}" "${RM_NODES:-?}"
	printf "    re-mastered : enq_status=%s lkid=%s is_local_master=%s master_csid_after=%s remastered=%s\n" \
		"${RM_ENQ_STATUS:-?}" "${RM_LKID:-?}" "${RM_LOCAL:-?}" \
		"${RM_MC_AFTER:-?}" "${RM_REMASTERED:-?}"
	echo ""

	XFAIL=0
	if [ -z "$RM_BEFORE_RES" ] || [ "$RM_BEFORE_RES" = "NONE" ]; then
		echo "  FAILED (1): node A found no resource its peer genuinely"
		echo "  mastered before departing -- nothing this phase can measure."
		XFAIL=1
	fi
	if [ "$RM_DEPARTED" != "1" ]; then
		echo "  FAILED (2): node A's own CLUB never dropped to one member --"
		echo "  the departure was never observed by this node's own"
		echo "  connectivity-loss ladder within the wait."
		XFAIL=1
	fi
	if [ "$RM_REMASTERED" != "1" ]; then
		echo "  FAILED (3): the same name did not re-master onto node A"
		echo "  (enq_status=$RM_ENQ_STATUS lkid=$RM_LKID is_local_master=$RM_LOCAL"
		echo "  master_csid_before=$RM_MC_BEFORE master_csid_after=$RM_MC_AFTER)."
		XFAIL=1
	fi
	if console_panicked A; then
		echo "  FAILED (4): node A's console shows a panic/bugcheck."
		XFAIL=1
	fi

	echo "=========================================="
	if [ "$XFAIL" = "0" ]; then
		echo "  AUTONOMOUS REMASTER PROOF PASSED (rd vms-1ee, DLM rung H10a"
		echo "  re-established, executive-resident):"
		echo "  node B joined, node A discovered a name node B genuinely"
		echo "  mastered, node B's process then exited with NO announcement"
		echo "  and no kill -9 -- node A's OWN connectivity-loss ladder"
		echo "  (RECNXINTERVAL's reconnect hold, then the coordinator's"
		echo "  transition) removed node B from membership entirely on its"
		echo "  own, that removal fired the DLM arm's member_departed"
		echo "  callback as a DIRECT CALL from cnxman_notify_membership_changes"
		echo "  (vms_cnxman.c) -- never an ioctl this rig issued on the"
		echo "  departed peer's behalf -- and the SAME name re-mastered onto"
		echo "  node A on its very next use. Autonomous, unassisted, and read"
		echo "  back from the executive at every step (INV-6)."
		echo "=========================================="
		exit 0
	fi
	echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null
	echo "--- node B console tail ---"; tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null
	exit 1
fi

# --------------------------------------------------------------------------
# rd vms-c06 BARRIER-RELEASE ASSERTION (HARD). Node A's own CLUB projection
# (RIG-A-CLUB, cluster_node.c print_club_line off
# vms_club_view_wire.transition_active) carries `transition`: 1 while the
# coordinator is inside a CNXMAN_COORD_BARRIER round, 0 once
# coord_try_release() has actually let it go. It is node A's OWN executive
# talking about its OWN coordinator -- a readback, not a rig label (INV-6).
#
# WHAT IT USED TO SAY, AND WHY IT IS A GATE NOW. Every genesis run measured
# before vms-c06 (tests/lab/captures/xnode-dlm-2node-20260911.log, RIG-A-CLUB
# from node B's admission through t=150 s+) showed this holding at 1 for the
# rest of the run: the coordinator opened the barrier for B's admission and
# never finished releasing it, because the joiner's op-0x0b step reports were
# being eaten one FSM short of it (cnxman_join_rx_body's "NOT OURS TO EAT").
# CN=2 was still reached, so it was reported as a non-failing diagnostic rather
# than hidden. vms-c06 fixed the release, and a barrier that stalls again is a
# real regression of a path a rejoin depends on -- so it now FAILS the run.
A_TRANSITION=$(rig_field A CLUB transition)
echo ""
if [ "$A_TRANSITION" = "0" ]; then
	echo "  BARRIER-RELEASE: REACHED (node A's own transition=0 -- the"
	echo "  coordinator's barrier round completed)"
else
	echo "  BARRIER-RELEASE: NOT REACHED (transition=${A_TRANSITION:-?})"
	echo "  The coordinator opened a barrier round for the admission and"
	echo "  never released it -- rd vms-c06's own defect, back. Read node A's"
	echo "  %CNXMAN transcript and the op-0x0b step reports on its CSB."
	echo "  GENESIS 2-NODE PROOF FAILED (barrier release)"
	echo "=========================================="
	echo "--- node A console tail ---"
	tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null
	echo "--- node B console tail ---"
	tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null
	exit 1
fi

if cn2_reached && [ "$A_ROLE" = "founder" ] && [ "$B_ROLE" = "joiner" ]; then
	echo "  GENESIS 2-NODE PROOF PASSED"
	echo "  Node A founded a VMScluster and node B joined it: both"
	echo "  executives report VMS_CLUSTER_MEMBER with CN=2."
	echo "=========================================="
	exit 0
fi

echo "  GENESIS 2-NODE PROOF FAILED"
echo "  A: role=${A_ROLE:-?} member=${A_MEMBER:-?} cn=${A_CN:-?} csid=${A_CSID:-?}"
echo "  B: role=${B_ROLE:-?} member=${B_MEMBER:-?} cn=${B_CN:-?} csid=${B_CSID:-?}"
if [ "$A_ROLE" = "founder" ] && [ "$A_MEMBER" = "1" ] && [ "$B_MEMBER" = "1" ]; then
	echo ""
	echo "  ADMISSION HELD, THE MEMBER COUNT DID NOT AGREE."
	echo "  Node A founded generation 1, admitted node B and counts"
	echo "  cn=${A_CN}; node B holds CSID ${B_CSID} -- which it can only have"
	echo "  computed from a generation it read out of a real op-0x06 -- and"
	echo "  its own executive reports MEMBER. What did NOT happen is node B"
	echo "  counting the OTHER member: no grounded wire field associates a"
	echo "  peer's SCSSYSTEMID with its CSID, so B holds no CSID for A, its"
	echo "  CSB for A cannot be matched to a nodemap bit, and B's own"
	echo "  executive says so -- '%CNXMAN, committed member count differs"
	echo "  from the transition nodemap'. See"
	echo "  docs/design-op06-membership-builder.md sec 8."
elif [ "$A_ROLE" = "founder" ] && [ "$A_MEMBER" = "1" ]; then
	echo "  (GENESIS itself HELD: node A founded with CSID ${A_CSID} and"
	echo "   its executive reports MEMBER. What did not happen is node B's"
	echo "   ADMISSION -- read RIG-B-JOINREC and the %CNXMAN transcript.)"
fi
echo "=========================================="
echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null
echo "--- node B console tail ---"; tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null
exit 1
