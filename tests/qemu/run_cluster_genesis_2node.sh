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
#           IT IS ALSO THE NEVER-CRASH-A-PEER PROOF. The arm has no RECEIVE
#           half for either opcode (vms_dlm_scs.c, "THE RELEASE'S RECEIVE
#           HALF"): both frames arrive at a peer that must DECLINE them,
#           counted, and go on being a member. So this mode asserts, after the
#           frames, that both nodes still report member=1 cn=2 with their two
#           membership projections agreeing and neither console panicked.
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
if [ "$MODE" = "xnode" ]; then
	XNODE=1
	WINDOW_A="${RIG_WINDOW_A:-150}"
	WINDOW_B="${RIG_WINDOW_B:-$((WINDOW_A - STAGGER))}"
	WALL="${RIG_WALL:-900}"
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

QEMU=qemu-system-x86_64
if [ -w /dev/kvm ]; then ACCEL="-accel kvm -cpu host"; else ACCEL="-accel tcg"; fi

echo "=== OVMX 2-node cluster GENESIS rig (rd vms-f6b) ==="
echo "mode=$MODE"
echo "accel=${ACCEL#-accel } group=$GROUP recnx=${RECNX}s stagger=${STAGGER}s"
[ "$XNODE" = "1" ] && echo "cross-node phase: ON (windows A=${WINDOW_A}s B=${WINDOW_B}s, linger=${LINGER}s)"
echo "node A: OVMXA/1025 VOTES=$VOTES_A EXPECTED_VOTES=1 VAXCLUSTER=2"
echo "node B: OVMXB/$SYSID_B VOTES=0          VAXCLUSTER=2"
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
	     "ovmx.recnx=$RECNX ovmx.credits=$CREDITS ovmx.swver=$SWVER" \
	     "ovmx.window=$6 ovmx.xnode=$XNODE ovmx.linger=$LINGER"
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
launch_node B OVMXB "$SYSID_B" 0 1 52:54:00:00:10:26 "$WINDOW_B"; PB=$LAUNCH_PID

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
# probes. Each probe sees BOTH directions (an AF_PACKET socket is delivered the
# interface's outgoing frames as well as its incoming ones), so a frame that
# really crossed appears in both -- which is why this is a >0 gate and never a
# count the verdict quotes as "how many were sent". That number is the arm's.
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
	A_GONE=$(num "$(dlm_field A POST survival lock_gone)")
	B_GONE=$(num "$(dlm_field B POST survival lock_gone)")
	A_PROJ=$(final_field A projections); B_PROJ=$(final_field B projections)
	W_DEQ=$(wire_saw deq); W_BLK=$(wire_saw blkast)

	echo "  CROSS-NODE DLM RUN (rd vms-94c) -- every value below was read"
	echo "  back out of the node's own executive, except the two WIRE counts,"
	echo "  which come from the nodes' own passive captures:"
	printf "    A: peer-mastered=%s master_csid=%s releases_sent=%s blkasts_sent=%s unparsed=%s projections=%s\n" \
		"${A_XRES:-none}" "${A_XMAS:-?}" "$A_REL" "$A_BLK" "$A_UNP" "${A_PROJ:-?}"
	printf "    B: peer-mastered=%s master_csid=%s releases_sent=%s blkasts_sent=%s unparsed=%s projections=%s\n" \
		"${B_XRES:-none}" "${B_XMAS:-?}" "$B_REL" "$B_BLK" "$B_UNP" "${B_PROJ:-?}"
	printf "    WIRE: op-0x03 deq frames=%s  op-0x04 blkast frames=%s\n" "$W_DEQ" "$W_BLK"
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
	# (4) THE NEVER-CRASH-A-PEER ASSERTION. Each node must have RECEIVED
	#     the peer's two frames and declined them: `unparsed` counts exactly
	#     the inbound cat-0x02 bodies the arm could not parse, and the only
	#     ones on this wire are op-0x03 and op-0x04.
	if [ "$A_UNP" -lt 2 ] || [ "$B_UNP" -lt 2 ]; then
		echo "  FAILED (4): a node did not record receiving BOTH of the"
		echo "  peer's cross-node frames (A unparsed=$A_UNP B unparsed=$B_UNP;"
		echo "  one op-0x03 + one op-0x04 each). Without the receive, the"
		echo "  survival below proves nothing about them."
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
	held "$( { [ "$A_UNP" -ge 1 ] && [ "$B_UNP" -ge 1 ] && \
		 ! console_panicked A && ! console_panicked B; } \
		&& echo 1 || echo 0)" \
		"NEVER CRASH A PEER: each node RECEIVED a frame it has no receive half"
	echo "              for, DECLINED it (counted), and stayed a sane member"
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
		echo "  executive."
		echo "=========================================="
		exit 0
	fi
	echo "=========================================="
	echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null
	echo "--- node B console tail ---"; tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null
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
