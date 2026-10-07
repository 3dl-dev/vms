#!/bin/bash
# test_decnetd_startnet_gate.sh - the DECnet startup GATE + serve-default
# decision SYS$MANAGER:STARTNET.COM relies on (rd vms-a70 direction B).
#
# STARTNET.COM starts the persistent NETACP daemon (SYS$SYSTEM:DECNETD.EXE)
# detached, with NO argv (VMS RUN passes an image no argv -- exactly as
# TCPIP$STARTUP starts TCPIP$INETD, which reads its own service DB). Two daemon
# behaviours make that work, and this test pins BOTH on the host (no CAP_NET_RAW,
# no /dev/vms, no wire -- --show-executor opens no socket):
#
#   1. THE GATE. `DECNETD --show-executor` SELF-SOURCES the executor address from
#      the node's DECnet configuration (executor.dat) and exits SUCCESS iff one
#      is configured, else %DECNETD-E-NOADDRESS. STARTNET branches on that exit
#      status: a node with no DECnet address starts no NETACP (INV-6 -- there is
#      NO unconditional success; an unconfigured node is honestly not running
#      DECnet, exactly as real VMS starts nothing until NCP DEFINE EXECUTOR).
#
#   2. THE SERVE DECISION. The persistent endnode daemon SERVES inbound $ SET
#      HOST (Session Control object 42 -> LOGINOUT on an executive-minted RTAn:)
#      by DEFAULT -- serving object 42 is what a NETACP does, and the daemon
#      cannot be told a mode flag through RUN. A --router or --no-cterm-server
#      invocation does NOT serve; an explicit --cterm-server pins it on.
#      --show-executor reports the decision honestly as a dry run.
#
# Exit 0 = all checks pass, 1 = a failure.
set -uo pipefail

BIN="${DECNETD_EXE:-}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    # Fall back to the conventional build location so the script also runs by
    # hand; never a fake pass -- if the binary is genuinely missing, fail.
    BIN="$(dirname "$0")/../../build/bin/DECNETD.EXE"
fi
if [ ! -x "$BIN" ]; then
    echo "FAIL: DECNETD.EXE not found (set DECNETD_EXE or build decnetd_exe first)"
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export OVMX_DECNET_EXECUTOR="$TMP/executor.dat"
export OVMX_DECNET_NODEDB="$TMP/netnode_remote.dat"

PASS=0 FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

# --- 1. GATE OFF: no executor.dat -> NOADDRESS, nonzero exit (STARTNET skips) ---
rm -f "$OVMX_DECNET_EXECUTOR"
out="$("$BIN" --show-executor 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q "NOADDRESS"; then
    ok "unconfigured node: --show-executor exits nonzero with %DECNETD-E-NOADDRESS (STARTNET starts no NETACP)"
else
    bad "unconfigured node should exit nonzero + NOADDRESS (rc=$rc): $out"
fi

# --- 2. GATE ON: executor.dat present -> self-source, exit 0 (STARTNET starts) ---
printf 'EXECUTOR 1.42 NAME OVMXR3 STATE ON\n' > "$OVMX_DECNET_EXECUTOR"
out="$("$BIN" --show-executor 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "Executor node = 1.42"; then
    ok "configured node: --show-executor self-sources address 1.42 and exits SUCCESS (STARTNET starts NETACP)"
else
    bad "configured node should self-source 1.42 + exit 0 (rc=$rc): $out"
fi

# --- 3. SERVE DECISION matrix (all self-source the configured address) ---------
serve()   { "$BIN" --show-executor "$@" 2>&1 | grep "Inbound SET HOST"; }
check_serve() { # desc, expected-substring, args...
    local desc="$1" want="$2"; shift 2
    local line; line="$(serve "$@")"
    if echo "$line" | grep -q "$want"; then ok "$desc"; else bad "$desc (got: $line)"; fi
}
check_serve "endnode daemon SERVES inbound object 42 by default (NETACP)"        "= served"
check_serve "a --router does NOT serve inbound (routing only)"                   "= not served" --router
check_serve "--no-cterm-server forces serve OFF"                                 "= not served" --no-cterm-server
check_serve "an explicit --router --cterm-server DOES serve"                     "= served"     --router --cterm-server

# --- 3b. IFACE resolution (gap B): argv-less daemon auto-detects the primary NIC
#         instead of the compile-time "br0" (which is absent in a booted netns) ----
iface_line() { "$BIN" --show-executor "$@" 2>&1 | grep "Datalink interface"; }
# explicit --iface is echoed verbatim and tagged (--iface)
if iface_line --iface lo | grep -q "= lo (--iface)"; then
    ok "explicit --iface is honoured and reported"
else
    bad "explicit --iface lo not reported (got: $(iface_line --iface lo))"
fi
# no --iface: the auto-detect SELECTION LOGIC, tested against a controlled
# interface list (LD_PRELOAD getifaddrs shim) rather than the CI host's real
# NICs -- the real set churns (docker veths appear/vanish under parallel jobs, a
# sandbox may hold only lo), which made the old "a real NIC exists in /sys"
# assertion intermittent (vms-94b). The shim is compiled here; a missing
# compiler or an ineffective shim is a FAIL, never a skip.
SHIM="$TMP/fake_getifaddrs.so"
SHIM_SRC="$(dirname "$0")/fake_getifaddrs.c"
if cc -shared -fPIC -o "$SHIM" "$SHIM_SRC" -ldl 2>"$TMP/shim.err"; then
    shim_iface() { FAKE_IFS="$1" LD_PRELOAD="$SHIM" "$BIN" --show-executor 2>&1 | grep "Datalink interface"; }
    got="$(shim_iface 'lo:lo,tun0:other,eth9:eth,eth10:eth')"
    if [ "$got" = "Datalink interface = eth9 (auto-detected primary NIC)" ]; then
        ok "no --iface: skips loopback and non-Ethernet netdevs, auto-detects the first Ethernet NIC (eth9)"
    else
        bad "auto-detect should pick eth9 from lo,tun0(non-ether),eth9,eth10 (got: $got)"
    fi
    got="$(shim_iface 'lo:lo,tun0:other')"
    if echo "$got" | grep -q "no usable NIC detected" && ! echo "$got" | grep -q "auto-detected primary NIC"; then
        ok "no usable NIC: reports the compiled default honestly, not as auto-detected (INV-6)"
    else
        bad "with no usable NIC the readout must not claim auto-detection (got: $got)"
    fi
else
    bad "could not compile the getifaddrs shim: $(cat "$TMP/shim.err")"
fi
# Real host: whatever is detected must never be loopback (host-independent).
auto="$(iface_line | sed -n 's/^Datalink interface = \([^ ]*\) .*/\1/p')"
if [ -n "$auto" ] && [ "$auto" != "lo" ]; then
    ok "no --iface on the real host never resolves to loopback ($auto)"
else
    bad "real-host auto-detect resolved to '$auto' (line: $(iface_line))"
fi

# --- 3c. DATALINK BACKEND readout (rd vms-1f69): the dry run names, honestly,
#          which raw-L2 path this binary was BUILT with -- the booted runtime's
#          "executive" (/dev/vms + PHY_IO), the host/lab "AF_PACKET probe", or
#          NetBSD "bpf" -- and where the executor database lives. -----------------
dl="$("$BIN" --show-executor 2>&1 | sed -n 's/^Datalink backend = //p')"
case "$dl" in
    executive|"AF_PACKET probe"|bpf) ok "--show-executor names the datalink backend ($dl)";;
    *) bad "--show-executor should name the datalink backend (got: '$dl')";;
esac
"$BIN" --show-executor 2>&1 | grep -q "^Executor database = $OVMX_DECNET_EXECUTOR\$" \
    && ok "--show-executor names where the executor database lives (the host hook here; SYS\$SYSTEM:NETNODE_LOCAL.DAT on the runtime)" \
    || bad "--show-executor should report the executor database location"

# --- 4. INV-6: a garbage executor.dat is NOT a configured address ---------------
printf 'garbage not an executor line\n' > "$OVMX_DECNET_EXECUTOR"
out="$("$BIN" --show-executor 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q "NOADDRESS"; then
    ok "a malformed executor.dat yields NOADDRESS, never an invented address (INV-6)"
else
    bad "malformed executor.dat should NOT be accepted (rc=$rc): $out"
fi

# --- 5. HELP documents the new contract ----------------------------------------
help="$("$BIN" --help 2>&1)"
echo "$help" | grep -q -- "--no-cterm-server" && ok "--help documents --no-cterm-server" || bad "--help missing --no-cterm-server"
echo "$help" | grep -qi "SELF-SOURCED"        && ok "--help documents executor-address self-sourcing" || bad "--help missing self-source note"

echo "----"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
