#!/bin/bash
# decnet_set_host_live.sh — prove a REAL interactive $ SET HOST / CTERM terminal
# session over a REAL datalink (rd vms-aac0 live bracket, child of vms-30e /
# vms-4d2).
#
# The --set-host-selftest proves the CTERM choreography over a socketpair with
# SCRIPTED payloads. This test proves the daemon carries a GENUINE interactive
# program's I/O over a real kernel L2 datalink: a --cterm-server HOST spawns a
# real PTY + a real login-command, and a --set-host TERMINAL client feeds real
# stdin and prints real stdout. NOTHING is canned — the "Welcome, <name>." line
# the client receives can ONLY be produced by the spawned program reading the
# client's input and transforming it, and the program independently records the
# input it read to a file we assert on. Both directions are proven.
#
# It stands up a veth pair (two real Ethernet endpoints), gives each end its
# faithful DECnet MAC (AA-00-04-00-<LE addr>), and runs two real DECNETD.EXE
# engines over that wire. Every CTERM/NSP field is the FSM's own (no template
# copying). Needs CAP_NET_ADMIN (veth + MAC) and CAP_NET_RAW (AF_PACKET); if it
# cannot create the veth it SKIPS honestly (exit 77) — never a fake pass.
#
# Build DECNETD.EXE first (static so it runs anywhere), then:
#   REPO=<repo> DECNETD_BIN=<path/DECNETD.EXE> tests/integration/decnet_set_host_live.sh
set -u
REPO="${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}"
BIN="${DECNETD_BIN:-$REPO/build/bin/DECNETD.EXE}"
V0=dneth0 V1=dneth1                      # veth ends (distinct from the nsp test)
MAC0=aa:00:04:00:2b:04                   # 1.43 (server)  area 1 node 43
MAC1=aa:00:04:00:2c:04                   # 1.44 (client)  area 1 node 44
WORK="$(mktemp -d)"
SRV_LOG="$WORK/server.log"; CLI_LOG="$WORK/client.log"; CAP="$WORK/wire.txt"
LOGIN="$WORK/login.sh"                    # the REAL interactive login-command
GOT="$WORK/got_input.txt"                 # side-channel proof of input receipt

cleanup() {
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null
    [ -n "${CAP_PID:-}" ] && kill "$CAP_PID" 2>/dev/null
    ip link del "$V0" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

[ -x "$BIN" ] || { echo "SKIP: no DECNETD.EXE at $BIN (build it first)"; exit 77; }

# The login-command: a genuine interactive program. It prints a prompt, READS a
# line from its stdin (the pty, fed by the client's keystrokes over CTERM), and
# prints a line DERIVED from that input, then exits. It also records the line it
# read to $GOT so we can prove input receipt independently of the return path.
cat > "$LOGIN" <<EOF
#!/bin/sh
printf 'Username: '
read line
printf '%s\n' "\$line" > "$GOT"
printf 'Welcome, %s.\n' "\$line"
EOF
chmod +x "$LOGIN"

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

# Server (1.43): CTERM HOST spawning the real login-command on a real PTY.
"$BIN" --address 1.43 --name OVMXS --iface "$V0" --cterm-server \
       --login-command "/bin/sh $LOGIN" --duration 15 > "$SRV_LOG" 2>&1 & SRV_PID=$!
sleep 1

# Client (1.44): $ SET HOST 1.43, feeding a REAL line of stdin. The client exits
# early when the host unbinds (the login-command exited); --duration bounds it.
printf 'SYSTEM\n' | \
    "$BIN" --address 1.44 --name OVMXC --iface "$V1" --set-host 1.43 \
           --duration 12 > "$CLI_LOG" 2>&1
sleep 1
kill "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null

echo "===== server ($MAC0, 1.43 — CTERM HOST) ====="; grep -E 'DECNETD-(I|W)-' "$SRV_LOG"
echo "===== client ($MAC1, 1.44 — SET HOST) ====="; cat "$CLI_LOG"
[ -s "$GOT" ] && { echo "===== program read (side-channel) ====="; cat "$GOT"; }
[ -s "$CAP" ] && { echo "===== wire (0x6003) ====="; head -8 "$CAP"; }

# Verdict: the FULL bidirectional pump must be proven over the real datalink.
fail=0
# --- Server-side: it accepted the CTERM object and spawned the real program.
grep -q 'DECNETD-I-CONNIN'  "$SRV_LOG" || { echo "MISSING: server did not receive the Connect Initiate"; fail=1; }
grep -q 'login-command spawned' "$SRV_LOG" || { echo "MISSING: server did not spawn the login-command"; fail=1; }
grep -q 'DECNETD-I-BOUND'   "$SRV_LOG" || { echo "MISSING: CTERM session never bound on the host"; fail=1; }
# --- Client input reached the program (side-channel, independent of the return path).
[ -s "$GOT" ] && grep -q '^SYSTEM$' "$GOT" || { echo "MISSING: the program did not read the client's input"; fail=1; }
# --- Client link came up and bound.
grep -q 'DECNETD-I-LINKUP' "$CLI_LOG" || { echo "MISSING: client link never reached RUN"; fail=1; }
grep -q 'DECNETD-I-BOUND'  "$CLI_LOG" || { echo "MISSING: client CTERM session never bound"; fail=1; }
# --- Server program output reached the client's stdout (both the prompt it
#     printed AND the input-derived line only the program can produce).
grep -q 'Username: '        "$CLI_LOG" || { echo "MISSING: host prompt did not reach the client"; fail=1; }
grep -q 'Welcome, SYSTEM\.' "$CLI_LOG" || { echo "MISSING: input-derived program output did not reach the client"; fail=1; }
# --- The session tore down honestly on the child exit.
grep -q 'DECNETD-I-CHILDEXIT' "$SRV_LOG" || { echo "MISSING: server did not detect the login-command exit"; fail=1; }

if [ "$fail" -eq 0 ]; then
    echo "PASS: a REAL interactive program's I/O crossed a live \$ SET HOST/CTERM"
    echo "      session over a REAL veth datalink — client input reached the"
    echo "      program AND the program's real output reached the client's stdout"
    exit 0
fi
echo "FAIL: the interactive CTERM session did not complete over the live datalink"
exit 1
