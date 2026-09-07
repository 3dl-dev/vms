#!/bin/bash
# decnet_nsp_live_datalink.sh — prove an NSP logical link over a REAL datalink
# (rd vms-c23: "NSP logical-link connection service ... over the live datalink").
#
# The socketpair selftest (decnetd --nsp-selftest) proves the FSM in isolation;
# it does NOT prove the daemon moves NSP PDUs over a real kernel L2 datalink.
# This test does: it stands up a veth pair (two real Ethernet endpoints; a frame
# transmitted on one end is received on the other), gives each end its faithful
# DECnet MAC (AA-00-04-00-<LE addr>, exactly what a real Phase IV node programs
# onto its NIC), and runs two real DECNETD.EXE engines — a --listen server and a
# --connect client — that carry a full CI -> CC -> DATA+ACK -> DI/DC logical
# link over that wire. Every field is the link FSM's own (executive-backed, no
# frame-template copying); nothing is a socketpair or an in-process shortcut.
#
# Needs CAP_NET_ADMIN (veth + MAC) and CAP_NET_RAW (AF_PACKET). Run it in a
# privileged container or netns; it creates its own veth and cleans up. If it
# cannot create the veth it SKIPS honestly (exit 77) rather than faking a pass.
#
# Build DECNETD.EXE first (static, so it runs anywhere):
#   cmake -S <repo> -B build -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DOVMX_STATIC=ON
#   cmake --build build --target decnetd_exe    # -> build/bin/DECNETD.EXE
set -u
REPO="${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}"
BIN="${DECNETD_BIN:-$REPO/build/bin/DECNETD.EXE}"
V0=dnet0 V1=dnet1                       # veth ends
MAC0=aa:00:04:00:2b:04                  # 1.43 (server)  area 1 node 43
MAC1=aa:00:04:00:2c:04                  # 1.44 (client)  area 1 node 44
WORK="$(mktemp -d)"
SRV_LOG="$WORK/server.log"; CLI_LOG="$WORK/client.log"; CAP="$WORK/wire.txt"

cleanup() {
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null
    [ -n "${CAP_PID:-}" ] && kill "$CAP_PID" 2>/dev/null
    ip link del "$V0" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

[ -x "$BIN" ] || { echo "SKIP: no DECNETD.EXE at $BIN (build it first)"; exit 77; }

# Stand up the veth pair with faithful DECnet MACs. If we lack the privilege,
# skip honestly -- never report a pass we did not earn.
if ! ip link add "$V0" type veth peer name "$V1" 2>/dev/null; then
    echo "SKIP: cannot create veth (need CAP_NET_ADMIN / privileged container)"
    exit 77
fi
ip link set "$V0" address "$MAC0" 2>/dev/null || { echo "SKIP: cannot set MAC"; exit 77; }
ip link set "$V1" address "$MAC1" 2>/dev/null || { echo "SKIP: cannot set MAC"; exit 77; }
ip link set "$V0" up; ip link set "$V1" up

command -v tcpdump >/dev/null 2>&1 && \
    tcpdump -i "$V0" -n -e -tt 'ether proto 0x6003' > "$CAP" 2>/dev/null & CAP_PID=$!

# Server on the 1.43 end; client on the 1.44 end. Each binds its own veth end,
# owns its DECnet MAC, and receives its unicast natively.
"$BIN" --address 1.43 --name OVMXS --iface "$V0" --listen --duration 12 > "$SRV_LOG" 2>&1 & SRV_PID=$!
sleep 1
"$BIN" --address 1.44 --name OVMXC --iface "$V1" --connect 1.43 --object 42 \
       --connect-data 'OVMX-NSP-LIVE-DATALINK-PROOF' --duration 10 > "$CLI_LOG" 2>&1
sleep 1
kill "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null

echo "===== server ($MAC0, 1.43) ====="; grep -E 'DECNETD-(I|W)-' "$SRV_LOG"
echo "===== client ($MAC1, 1.44) ====="; grep -E 'DECNETD-(I|W)-' "$CLI_LOG"
[ -s "$CAP" ] && { echo "===== wire (0x6003) ====="; grep -iE 'conn-initiate|conn-confirm|data|ack|disconn' "$CAP" | head -8; }

# Verdict: BOTH ends must show the full lifecycle over the real datalink.
fail=0
grep -q 'DECNETD-I-CONNIN'  "$SRV_LOG" || { echo "MISSING: server did not receive the Connect Initiate"; fail=1; }
grep -q 'DECNETD-I-LINKUP'  "$SRV_LOG" || { echo "MISSING: server link never reached RUN"; fail=1; }
grep -q 'OVMX-NSP-LIVE-DATALINK-PROOF' "$SRV_LOG" || { echo "MISSING: data segment not delivered byte-identical"; fail=1; }
grep -q 'DECNETD-I-LINKUP'  "$CLI_LOG" || { echo "MISSING: client link never reached RUN (no Connect Confirm)"; fail=1; }
grep -q 'DECNETD-I-DATATX'  "$CLI_LOG" || { echo "MISSING: client never sent data"; fail=1; }
grep -q 'DECNETD-I-LINKDOWN' "$CLI_LOG" || { echo "MISSING: link did not close cleanly"; fail=1; }

if [ "$fail" -eq 0 ]; then
    echo "PASS: NSP logical link OPEN -> DATA (byte-identical) -> DISCONNECT over a REAL veth datalink"
    exit 0
fi
echo "FAIL: NSP did not complete over the live datalink"
exit 1
