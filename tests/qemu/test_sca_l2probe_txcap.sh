#!/bin/bash
# test_sca_l2probe_txcap.sh - the TRANSMIT-CAPTURE positive control (rd
# vms-175).
#
# WHAT THIS PROVES. Before this item, sca_l2probe's `recv` mode bound
# AF_PACKET to the SPECIFIC ethertype 0x6007, which the Linux kernel only
# fans out to `ptype_base` listeners on the RECEIVE path (netif_receive_skb).
# A locally transmitted frame is announced only through the SEPARATE
# `ptype_all` list inside dev_queue_xmit_nit() -- so that build's own capture
# of ITS OWN NODE could never show a self-sourced connect, no matter whether
# the node opened one. "connect_req_from_self=0" was true either way: a
# vacuous gate (the exact defect this item exists to close; see
# sca_l2probe.c's "WHY ETH_P_ALL, NOT 0x6007").
#
# This test is the FALSIFIABILITY proof: on a REAL (non-loopback) net device
# -- a veth end, which faithfully does NOT loop a locally transmitted frame
# back into its own receive path, exactly like the virtio-net devices the
# genesis rig's QEMU guests use -- it transmits a marker 0x6007 frame with
# `sca_l2probe send` while `sca_l2probe recv` is listening on the SAME
# device, and asserts the receive side's own SCA-L2PROBE-DONE line reports
# self_tx_frames>=1 (and every SCA-L2PROBE-RX line for that frame carries
# dir=TX). If self-transmit were still unobservable, this assertion would
# fail -- which is what makes "connect_req_from_self=0" trustworthy again.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/sca_l2probe.c"
BIN="$(mktemp -d)/sca_l2probe"

EXIT_SKIP=77
skip_honest() {
	echo "SKIP: $1"
	echo "-- this test needs CAP_NET_ADMIN (to create a veth pair) and"
	echo "   CAP_NET_RAW (for the AF_PACKET probe itself). No result is"
	echo "   fabricated for the skip."
	exit "$EXIT_SKIP"
}

command -v gcc >/dev/null 2>&1 || skip_honest "gcc not available"
command -v ip >/dev/null 2>&1 || skip_honest "no netlink tooling (ip)"
[ -f "$SRC" ] || skip_honest "$SRC not found"

gcc -O2 -Wall -Wextra -o "$BIN" "$SRC" || {
	echo "FAIL: sca_l2probe failed to build" >&2
	exit 1
}

VA="ovmxtxa$$"
VB="ovmxtxb$$"
cleanup() {
	sudo -n ip link del "$VA" >/dev/null 2>&1 || ip link del "$VA" >/dev/null 2>&1
	rm -rf "$(dirname "$BIN")"
}
trap cleanup EXIT

SUDO=""
if [ "$(id -u)" != "0" ]; then
	sudo -n true >/dev/null 2>&1 || skip_honest "not root and no passwordless sudo"
	SUDO="sudo -n"
fi

$SUDO ip link add "$VA" type veth peer name "$VB" >/dev/null 2>&1 \
	|| skip_honest "cannot create veth pair $VA/$VB (need CAP_NET_ADMIN)"
$SUDO ip link set "$VA" up >/dev/null 2>&1
$SUDO ip link set "$VB" up >/dev/null 2>&1

RECV_LOG="$(mktemp)"
# No pcap path: this test only needs the stdout census (self_tx_frames=,
# dir=TX), and a pcap opened by a privileged sub-process here would be
# owned by that process rather than this script's own user -- avoided
# entirely rather than fought.
$SUDO "$BIN" recv "$VA" 5 > "$RECV_LOG" 2>&1 &
RECV_PID=$!
sleep 1

SEND_LOG="$(mktemp)"
$SUDO "$BIN" send "$VA" 3 200 TXCAP-PROOF > "$SEND_LOG" 2>&1
SEND_RC=$?

wait "$RECV_PID"

echo "=== sca_l2probe send (on $VA) ==="
cat "$SEND_LOG"
echo "=== sca_l2probe recv (on $VA, SAME node/device) ==="
cat "$RECV_LOG"

FAIL=0
if [ "$SEND_RC" != "0" ]; then
	echo "  FAIL: sca_l2probe send exited $SEND_RC (needs CAP_NET_RAW)"
	FAIL=1
fi

SELF_TX=$(sed -n 's/.*self_tx_frames=\([0-9]*\).*/\1/p' "$RECV_LOG" | tail -n1)
SELF_TX=${SELF_TX:-0}
TX_LINES=$(grep -c 'SCA-L2PROBE-RX, self .* dir=TX' "$RECV_LOG" 2>/dev/null || true)
TX_LINES=${TX_LINES:-0}

if [ "$SELF_TX" -lt 1 ] 2>/dev/null; then
	echo "  FAIL: self_tx_frames=$SELF_TX -- this node's own transmitted"
	echo "  0x6007 frame was NOT observed in its own capture. The"
	echo "  TRANSMIT-CAPTURE fix (rd vms-175) is not working: a self-sourced"
	echo "  CONNECT_REQ would again be invisible to the rig, and"
	echo "  'connect_req_from_self=0' would again be vacuous."
	FAIL=1
else
	echo "  HELD: self_tx_frames=$SELF_TX -- this node's own TX is observable"
	echo "  in its own capture (positive control for the falsifiability"
	echo "  claim: a self-sourced CONNECT_REQ, were one to occur, MUST show"
	echo "  up here)."
fi

if [ "$TX_LINES" -lt 1 ] 2>/dev/null; then
	echo "  FAIL: no SCA-L2PROBE-RX line reports dir=TX for a self-sourced"
	echo "  frame."
	FAIL=1
fi

if ! grep -q "SCA-L2PROBE-SELFTX-OK" "$RECV_LOG"; then
	echo "  FAIL: the recv side never logged SCA-L2PROBE-SELFTX-OK."
	FAIL=1
fi

rm -f "$RECV_LOG" "$SEND_LOG"

if [ "$FAIL" != "0" ]; then
	echo "TXCAP-PROOF: FAILED"
	exit 1
fi
echo "TXCAP-PROOF: PASSED (rd vms-175 transmit-capture fix)"
exit 0
