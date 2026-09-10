#!/bin/bash
# decnet_set_host_live.sh — prove a REAL interactive $ SET HOST / CTERM terminal
# session over a REAL datalink, CLIENT -> AUTHENTICATED inbound (rd vms-f54).
#
# This is the CLIENT proof for the OUTBOUND half of $ SET HOST. It stands up a
# veth pair (two real Ethernet endpoints), gives each end its faithful DECnet MAC
# (AA-00-04-00-<LE addr>), and runs two real DECNETD.EXE engines over that wire:
#
#   * the SERVER is main's $CREPRC-based inbound (--cterm-server): an inbound
#     connect to Session Control object 42 mints an RTAn: in the executive and
#     $CREPRCs LOGINOUT.EXE onto it, so the remote user is AUTHENTICATED FRESH.
#     It spawns NO shell and holds NO credential (the P4/P5 model, vms-f40/9ab).
#   * the CLIENT is --set-host (this item): it opens the CTERM terminal session,
#     bridges its VMS terminal channel to the link, and returns with the
#     canonical %REM-S-END on teardown.
#
# Every CTERM/NSP field is the FSM's own (no template copying). Needs
# CAP_NET_ADMIN (veth + MAC) and CAP_NET_RAW (AF_PACKET). The AUTHENTICATED leg
# additionally needs a real executive (/dev/vms) + SYS$SYSTEM:LOGINOUT.EXE for
# the server's $CREPRC:
#
#   * FULL proof (executive present): the client reaches the remote's FRESH
#     LOGINOUT challenge ("Username:") over the wire -- proof it is authenticated
#     fresh and the carried --user is proxy only, never auto-login -- and control
#     returns with %REM-S-END.
#   * REDUCED honest proof (no executive here): the client's logical link reaches
#     the server, which HONESTLY refuses (DECNETD-E-NOSESSION) because it cannot
#     $CREPRC without an executive -- proving there is no unauthenticated shell
#     fallback (INV-6) -- and the authenticated assertions are reported SKIPPED,
#     never faked.
#
# If it cannot create the veth it SKIPS honestly (exit 77) — never a fake pass.
#
#   REPO=<repo> DECNETD_BIN=<path/DECNETD.EXE> tests/integration/decnet_set_host_live.sh
set -u
REPO="${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}"
BIN="${DECNETD_BIN:-$REPO/build/bin/DECNETD.EXE}"
V0=dneth0 V1=dneth1                      # veth ends (distinct from the nsp test)
MAC0=aa:00:04:00:2b:04                   # 1.43 (server)  area 1 node 43
MAC1=aa:00:04:00:2c:04                   # 1.44 (client)  area 1 node 44
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

# Is a real executive present? The server's $CREPRC->LOGINOUT authentication
# needs it; without it we run the reduced honest proof.
HAVE_EXEC=0
[ -e /dev/vms ] && HAVE_EXEC=1

# Stand up the veth pair with faithful DECnet MACs, or skip honestly.
if ! ip link add "$V0" type veth peer name "$V1" 2>/dev/null; then
    echo "SKIP: cannot create veth (need CAP_NET_ADMIN / privileged container)"
    exit 77
fi
ip link set "$V0" address "$MAC0" 2>/dev/null || { echo "SKIP: cannot set MAC"; exit 77; }
ip link set "$V1" address "$MAC1" 2>/dev/null || { echo "SKIP: cannot set MAC"; exit 77; }
ip link set "$V0" up; ip link set "$V1" up

command -v tcpdump >/dev/null 2>&1 && \
    tcpdump -i "$V0" -n -e -tt 'ether proto 0x6003' > "$CAP" 2>/dev/null & CAP_PID=$!

# Server (1.43): main's $CREPRC-based CTERM HOST. It self-sources nothing; the
# address is explicit here (the daemon's own CLI), interface pinned to the veth.
"$BIN" --address 1.43 --name OVMXS --iface "$V0" --cterm-server \
       --duration 15 > "$SRV_LOG" 2>&1 & SRV_PID=$!
sleep 1

# Client (1.44): $ SET HOST 1.43. Feed a username then a (deliberately wrong)
# password so the exchange exercises the FRESH LOGINOUT challenge without ever
# auto-logging-in on the carried --user. --duration bounds it.
printf 'SYSTEM\nwrongpass\n' | \
    "$BIN" --address 1.44 --name OVMXC --iface "$V1" --set-host 1.43 \
           --user SYSTEM --duration 12 > "$CLI_LOG" 2>&1
sleep 1
kill "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null

echo "===== server ($MAC0, 1.43 — CTERM HOST via \$CREPRC->LOGINOUT) ====="
grep -E 'DECNETD-(I|W|E)-' "$SRV_LOG"
echo "===== client ($MAC1, 1.44 — SET HOST) ====="; cat "$CLI_LOG"
[ -s "$CAP" ] && { echo "===== wire (0x6003) ====="; head -8 "$CAP"; }

fail=0

# --- ALWAYS PROVEN (needs only CAP_NET): the CLIENT drove a REAL Connect
#     Initiate to Session Control object 42 across the veth datalink.
grep -q 'DECNETD-I-SETHOST' "$CLI_LOG" || { echo "MISSING: client did not send the Connect Initiate"; fail=1; }

if [ "$HAVE_EXEC" -eq 1 ]; then
    # --- FULL authenticated proof: the server accepted and $CREPRC'd LOGINOUT,
    #     and the client rode the session (link RUN) to the FRESH challenge +
    #     %REM-S-END.
    grep -q 'DECNETD-I-SESSTART' "$SRV_LOG" || { echo "MISSING: server did not accept + \$CREPRC LOGINOUT"; fail=1; }
    grep -q 'DECNETD-I-LINKUP'   "$CLI_LOG" || { echo "MISSING: client link never reached RUN over the wire"; fail=1; }
    grep -q 'DECNETD-I-BOUND'    "$CLI_LOG" || { echo "MISSING: client CTERM session never bound"; fail=1; }
    # The FRESH LOGINOUT challenge can only be produced by the real authenticator
    # on the remote -- proof the carried --user did NOT auto-login.
    grep -qi 'Username:' "$CLI_LOG" || { echo "MISSING: the remote LOGINOUT did not challenge the client (fresh auth)"; fail=1; }
    # Control returned with the canonical VMS message.
    grep -q '%REM-S-END, control returned to node' "$CLI_LOG" || { echo "MISSING: canonical %REM-S-END on teardown"; fail=1; }

    if [ "$fail" -eq 0 ]; then
        echo "PASS: an OVMX \$ SET HOST reached the remote's AUTHENTICATED LOGINOUT"
        echo "      over a REAL veth datalink (fresh Username challenge, carried"
        echo "      --user is proxy only), and control returned with %REM-S-END."
        exit 0
    fi
    echo "FAIL: the authenticated CLIENT session did not complete over the live datalink"
    exit 1
else
    # --- REDUCED honest proof (no executive): the CLIENT's Connect Initiate
    #     reached the server, which HONESTLY refused because it cannot $CREPRC
    #     without an executive -- no unauthenticated shell fallback (INV-6) -- so
    #     the client's link never reaches RUN and no login is ever fabricated.
    grep -q 'DECNETD-E-NOSESSION' "$SRV_LOG" || { echo "MISSING: server did not honestly refuse without an executive"; fail=1; }
    grep -q 'DECNETD-I-SESSTART'  "$SRV_LOG" && { echo "UNEXPECTED: server claims a session with no executive"; fail=1; }
    grep -q 'DECNETD-I-LINKUP'    "$CLI_LOG" && { echo "UNEXPECTED: client link reached RUN though the server refused"; fail=1; }
    grep -qi 'Username:'          "$CLI_LOG" && { echo "UNEXPECTED: a login challenge with no executive (fabrication)"; fail=1; }

    if [ "$fail" -eq 0 ]; then
        echo "SKIP-AUTH: no /dev/vms here — the CLIENT drove a real link to object 42"
        echo "  over the wire and the server refused honestly (DECNETD-E-NOSESSION);"
        echo "  the AUTHENTICATED LOGINOUT + %REM-S-END proof needs the executive leg"
        echo "  (fire it with: gh workflow run ci.yml)."
        exit 77
    fi
    echo "FAIL: the reduced (no-executive) proof did not hold"
    exit 1
fi
