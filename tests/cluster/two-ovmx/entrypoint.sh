#!/bin/bash
# entrypoint.sh -- the FIRST two-OVMX-node SCS harness (rd vms-f3e).
#
# Runs entirely inside THIS container's own network namespace: a Linux bridge
# (br0) with two veth endpoints, and TWO SCSD.EXE --connect processes, each
# bound (AF_PACKET/SOCK_RAW) to its OWN endpoint with a DISTINCT identity
# (SCSSYSTEMID + SCSNODE, hence distinct Con.ID base and distinct MAC). Both
# endpoints sit on the one L2 segment, so the 0x6007 LAVC/SCA multicast
# (AB-00-04-01-01-01) and every directed frame reach each other. A pcap on br0
# captures the whole exchange.
#
# This is NOT a VAX lab: no SIMH, no QEMU, no disk images. It is two OVMX
# userspace daemons talking SCS to EACH OTHER -- the OVMX<->OVMX join that the
# stop-and-wait sequencer in scsd.c was only ever tuned against real VAX peers.
#
# WHY veth, not taps (as tests/lab/entrypoint.sh uses): SIMH holds a tap's fd,
# which raises the tap's carrier. SCSD binds AF_PACKET directly and opens no tap
# fd, so a bare `ip tuntap`+`ip link set up` tap stays NO-CARRIER and the qdisc
# drops its egress. A veth pair carries as soon as BOTH ends are admin-up -- no
# fd needed -- so it is the correct model for two userspace AF_PACKET peers.
#
# Requires: CAP_NET_ADMIN (create bridge/veth) + CAP_NET_RAW (AF_PACKET). NOT
# privileged, NO /dev/net/tun. GitHub CI runners lack these caps today, so this
# runs on the workshop host via `docker run --cap-add NET_ADMIN --cap-add
# NET_RAW`, or a k3s pod granting the same two caps. See HARNESS.md.
set -u

OUT="${OUT_DIR:-/out}"
DURATION="${DURATION:-90}"
mkdir -p "$OUT"

# Identities: distinct SCSNODE + SCSSYSTEMID => distinct Con.ID base + logical
# LAVC addr. veth ends get distinct random HW MACs by default.
NODE_A="${NODE_A:-OVMXA}"; SYSID_A="${SYSID_A:-1601}"
NODE_B="${NODE_B:-OVMXB}"; SYSID_B="${SYSID_B:-1602}"
IF_A=v0a; IF_B=v1a

log() { echo "[two-ovmx] $*"; }

# --- 1. private L2 segment: br0 + two veth pairs, all in this netns -----------
log "building br0 + veth endpoints ($IF_A, $IF_B)"
ip link add br0 type bridge
# Non-IP L2 multicast (0x6007) must FLOOD; keep the bridge from snooping it away.
echo 0 > /sys/class/net/br0/bridge/multicast_snooping 2>/dev/null || true
ip link set br0 up
for pair in "v0a:v0b" "v1a:v1b"; do
  a="${pair%:*}"; b="${pair#*:}"
  ip link add "$a" type veth peer name "$b"
  ip link set "$b" master br0
  ip link set "$b" up
  ip link set "$a" up
  # The SCSD-bound end receives multicast/foreign frames flooded by the bridge.
  ip link set "$a" promisc on
  ip link set "$a" allmulticast on
done
ip -brief link show

MAC_A=$(cat /sys/class/net/$IF_A/address)
MAC_B=$(cat /sys/class/net/$IF_B/address)
log "$NODE_A -> $IF_A mac=$MAC_A sysid=$SYSID_A"
log "$NODE_B -> $IF_B mac=$MAC_B sysid=$SYSID_B"

# --- 2. per-node SYSGEN identity stores (hermetic, from scratch) -------------
STORE_A="$OUT/sysgen-$NODE_A.dat"; STORE_B="$OUT/sysgen-$NODE_B.dat"
python3 /harness/mk_sysgen_scratch.py "$STORE_A" "$NODE_A" "$SYSID_A" 1 20
python3 /harness/mk_sysgen_scratch.py "$STORE_B" "$NODE_B" "$SYSID_B" 1 20

export LD_LIBRARY_PATH=/harness/lib
SCSD=/harness/bin/SCSD.EXE
# Extra env passed straight through to BOTH daemons (e.g. OVMX_JOIN_SEQ=1 for a
# diagnosis run). Empty for the true baseline = UNMODIFIED main default flags.
EXTRA_ENV=( ${SCSD_ENV:-} )

# --- 3. capture on the bridge (sees BOTH directions of the exchange) ---------
PCAP="$OUT/two-ovmx.pcap"
tcpdump -i br0 -w "$PCAP" -U -s 0 'ether proto 0x6007' >"$OUT/tcpdump.log" 2>&1 &
TCPD=$!
sleep 1

# --- 4. two OVMX SCSD --connect daemons, one per endpoint --------------------
log "launching SCSD-A ($NODE_A) on $IF_A for ${DURATION}s"
env "${EXTRA_ENV[@]}" OVMX_SYSGEN_PATH="$STORE_A" \
    "$SCSD" --connect --duration "$DURATION" --iface "$IF_A" \
    >"$OUT/scsd-$NODE_A.log" 2>&1 &
PA=$!
log "launching SCSD-B ($NODE_B) on $IF_B for ${DURATION}s"
env "${EXTRA_ENV[@]}" OVMX_SYSGEN_PATH="$STORE_B" \
    "$SCSD" --connect --duration "$DURATION" --iface "$IF_B" \
    >"$OUT/scsd-$NODE_B.log" 2>&1 &
PB=$!

# --- 5. wait out the run, then reap cleanly ----------------------------------
# Hard ceiling so a hung daemon can never wedge the container.
HARD=$((DURATION + 30))
( sleep "$HARD"; kill -TERM "$PA" "$PB" 2>/dev/null ) &
GUARD=$!
wait "$PA" 2>/dev/null; wait "$PB" 2>/dev/null
kill "$GUARD" 2>/dev/null
sleep 1
kill -INT "$TCPD" 2>/dev/null; wait "$TCPD" 2>/dev/null

# --- 6. verdict --------------------------------------------------------------
FRAMES=$( (command -v tcpdump >/dev/null && tcpdump -r "$PCAP" 2>/dev/null | wc -l) || echo "?" )
bash /harness/verdict.sh "$OUT" "$NODE_A" "$NODE_B" "$FRAMES" | tee "$OUT/VERDICT.txt"
log "artifacts in $OUT: $(ls "$OUT" | tr '\n' ' ')"
