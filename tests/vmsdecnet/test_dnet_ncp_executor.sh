#!/bin/bash
# test_dnet_ncp_executor.sh - NCP.EXE EXECUTOR + NODE command dispatch, end to
# end against the shipped binary (rd vms-1f69). Drives SET/DEFINE/SHOW EXECUTOR
# and SET NODE / SHOW KNOWN NODES through the HOST-TEST hook
# (OVMX_DECNET_EXECUTOR / OVMX_DECNET_NODEDB), asserting persistence across
# separate invocations, the OVMX-layout label in the file, and honest refusals.
# On the booted runtime the same commands persist to SYS$SYSTEM:NETNODE_LOCAL.DAT
# / NETNODE_REMOTE.DAT through RMS over the ACP -- proven by the DECnet section
# of tests/qemu/lib/dcl_acceptance_battery.sh. (This script never runs NCP
# without the hook: on a host whose RMS layer has a /vms passthrough that would
# write host files.)
set -uo pipefail

NCP="${NCP_EXE:-}"
if [ -z "$NCP" ] || [ ! -x "$NCP" ]; then
    NCP="$(dirname "$0")/../../build/bin/NCP.EXE"
fi
if [ ! -x "$NCP" ]; then
    echo "FAIL: NCP.EXE not found (set NCP_EXE or build ncp_exe first)"; exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export OVMX_DECNET_EXECUTOR="$TMP/netnode_local.dat"
export OVMX_DECNET_NODEDB="$TMP/netnode_remote.dat"
export OVMX_DECNET_OBJECTDB="$TMP/netobject.dat"

PASS=0 FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
ncp() { "$NCP" "$@" 2>&1; }

# --- unconfigured: SHOW EXECUTOR says so, never an invented address ----------
out="$(ncp SHOW EXECUTOR)"
echo "$out" | grep -q "Executor node = not configured" \
    && ok "fresh node: SHOW EXECUTOR reports 'not configured' (no invented address)" \
    || bad "fresh node SHOW EXECUTOR: $out"

# --- SET EXECUTOR ADDRESS / NAME / STATE, each a SEPARATE invocation -----------
ncp SET EXECUTOR ADDRESS 1.42 >/dev/null; rc=$?
[ "$rc" -eq 0 ] && ok "SET EXECUTOR ADDRESS 1.42 exits 0" || bad "SET EXECUTOR ADDRESS exit $rc"
ncp SET EXECUTOR NAME OVMX >/dev/null; rc=$?
[ "$rc" -eq 0 ] && ok "SET EXECUTOR NAME OVMX exits 0" || bad "SET EXECUTOR NAME exit $rc"
ncp DEFINE EXECUTOR STATE ON >/dev/null; rc=$?
[ "$rc" -eq 0 ] && ok "DEFINE EXECUTOR STATE ON exits 0" || bad "DEFINE EXECUTOR STATE exit $rc"

out="$(ncp SHOW EXECUTOR)"
echo "$out" | grep -q "Executor node = 1.42 (OVMX)" \
    && ok "SHOW EXECUTOR (new invocation) shows 1.42 (OVMX) -- all three SETs persisted together" \
    || bad "SHOW EXECUTOR missing 1.42 (OVMX): $out"
echo "$out" | grep -qE "State +=  *on" \
    && ok "SHOW EXECUTOR shows State = on" || bad "SHOW EXECUTOR state: $out"

grep -q "OVMX layout, not the VMS NETNODE_LOCAL.DAT binary format" "$OVMX_DECNET_EXECUTOR" \
    && ok "the executor database labels its record layout as OVMX's (Rule 8)" \
    || bad "executor database lacks the OVMX-layout label: $(cat "$OVMX_DECNET_EXECUTOR")"

# --- honest refusals ----------------------------------------------------------
out="$(ncp SET EXECUTOR ADDRESS 64.1)"; rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -q "%NCP-E-INVADDR" \
    && ok "SET EXECUTOR ADDRESS 64.1 (area out of range) refused %NCP-E-INVADDR" \
    || bad "out-of-range address should be refused (rc=$rc): $out"
out="$(ncp SHOW EXECUTOR)"
echo "$out" | grep -q "1.42 (OVMX)" \
    && ok "a refused SET leaves the persisted executor untouched" \
    || bad "refused SET clobbered the executor: $out"

# a write that cannot land names the file and fails (CFGWRERR), never success
export OVMX_DECNET_EXECUTOR="$TMP/no-such-dir/netnode_local.dat"
out="$(ncp SET EXECUTOR ADDRESS 1.43)"; rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -q "%NCP-E-CFGWRERR" \
              && echo "$out" | grep -q "no-such-dir/netnode_local.dat" \
    && ok "an unwritable executor database fails %NCP-E-CFGWRERR naming the file" \
    || bad "unwritable executor database should fail honestly (rc=$rc): $out"
export OVMX_DECNET_EXECUTOR="$TMP/netnode_local.dat"

# --- node database through the same store -------------------------------------
ncp SET NODE 1.1 NAME VAX1 >/dev/null && ok "SET NODE 1.1 NAME VAX1 exits 0" || bad "SET NODE failed"
out="$(ncp SHOW KNOWN NODES)"
echo "$out" | grep -qE "^1\.1 +VAX1" \
    && ok "SHOW KNOWN NODES (new invocation) lists 1.1 VAX1" \
    || bad "SHOW KNOWN NODES: $out"
grep -q "OVMX layout, not VMS NETNODE_REMOTE.DAT" "$OVMX_DECNET_NODEDB" \
    && ok "the node database labels its layout as OVMX's" \
    || bad "node database lacks the OVMX-layout label"

# --- MAXIMUM LINKS (rd vms-f91): VMS default, SET, persistence, refusal --------
out="$(ncp SHOW EXECUTOR CHARACTERISTICS)"
echo "$out" | grep -qE "^Maximum links +=  *9$" \
    && ok "SHOW EXECUTOR CHARACTERISTICS shows the seeded 'Maximum links = 9' when none is set (rd vms-9cd)" \
    || bad "default Maximum links: $out"
ncp SET EXECUTOR MAXIMUM LINKS 12 >/dev/null; rc=$?
out="$(ncp SHOW EXECUTOR CHARACTERISTICS)"
[ "$rc" -eq 0 ] && echo "$out" | grep -qE "^Maximum links +=  *12$" && echo "$out" | grep -q "1.42 (OVMX)" \
    && ok "SET EXECUTOR MAXIMUM LINKS 12 persists (new invocation shows 12, address/name kept)" \
    || bad "SET EXECUTOR MAXIMUM LINKS 12: rc=$rc $out"
out="$(ncp SET EXECUTOR MAXIMUM LINKS 0)"; rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -q "%NCP-E-INVPVA" && ncp SHOW EXECUTOR CHARACTERISTICS | grep -qE "^Maximum links +=  *12$" \
    && ok "SET EXECUTOR MAXIMUM LINKS 0 is refused (INVPVA) and changes nothing" \
    || bad "MAXIMUM LINKS 0 not refused: rc=$rc $out"

echo "----"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
