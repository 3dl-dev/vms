#!/bin/bash
# decnet_adjacency.sh <pod> <tag> <duration> [OVMX_ADDR] [OVMX_NAME]
#
# DECnet Phase IV LIVE-ADJACENCY probe against a lab VAX oracle (rd vms-aac0,
# epic vms-30e; design-decnet-ovmx.md §5 Phase 1). Stages the userspace DECnet
# engine DECNETD.EXE onto a lab-2-style pod's br0 -- the SAME L2 segment a lab
# VAX runs DECnet Phase IV on -- runs it as an endnode for <duration> seconds,
# and captures raw 0x6003 (Phase IV ethertype cannot traverse SLIRP; needs the
# pod br0 the lab VAX taps into). Reports, from GROUND TRUTH on the wire + the
# engine's own adjacency table:
#   - OVMX emitted faithful endnode-HELLOs on the real segment (src OVMX_ADDR),
#   - which real VAX node(s) OVMX HEARD and registered (engine SHOW ADJACENT),
#   - each observed node's routing state (the endnode-HELLO `rtr` field): a lab
#     endnode with `rtr 0.0` has no router and will NOT form a hello-only
#     adjacency back to an OVMX endnode (Phase IV: endnodes peer with routers,
#     not each other) -- this probe surfaces exactly that, honestly.
#
# WHAT THIS PROVES / DOES NOT PROVE. It proves the userspace engine runs on a
# REAL lab wire against the REAL oracle and forms an adjacency entry for it,
# with zero disruption (it emits only the oracle-verified endnode-HELLO, the
# most benign Phase IV frame -- ovmx-never-crashes-a-peer). It does NOT by
# itself make the VAX list OVMX ("reachable endnode", the vms-aac0 done-bar):
# that needs OVMX reachable via the live NSP logical link (the NSP service is
# landed but socketpair-only; wiring it into decnetd's live loop is the next
# rung) and/or a node definition on the VAX. See
# tests/lab/captures/decnet-aac0-endnode-probe-20260907.md.
#
# ⚠ SHARED POD. If the pod also runs a VMScluster, DECnet (0x6003) is protocol-
# orthogonal to SCS (0x6007): this probe never touches cluster membership. But
# coordinate: one 0x6003 talker at a time, and never restart the pod.
#
# Build DECNETD.EXE (static, portable into the glibc pod):
#   cmake -S <repo> -B /tmp/b -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DOVMX_STATIC=ON
#   cmake --build /tmp/b --target decnetd_exe
#   -> /tmp/b/bin/DECNETD.EXE   (override via DECNETD_BIN=)
set -u
POD="${1:?usage: decnet_adjacency.sh <pod> <tag> <duration> [OVMX_ADDR] [OVMX_NAME]}"
TAG="${2:?tag required (unique per run)}"
DUR="${3:?duration seconds required}"
ADDR="${4:-1.42}"
NAME="${5:-OVMX}"

NS="${NS:-ovmx-lab}"
L="${L:-/lab/k8s-labs/$POD/logs}"
REPO="${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}"
DECNETD_BIN="${DECNETD_BIN:-$REPO/build/bin/DECNETD.EXE}"
IFACE="${IFACE:-br0}"
RDIR="/lab/run-$TAG"

log(){ echo "[$(date +%T)] $*"; }
kx(){ kubectl -n "$NS" exec "$POD" -- "$@"; }

kubectl -n "$NS" get pod "$POD" >/dev/null 2>&1 || { echo "FATAL: no pod $POD" >&2; exit 2; }
[ -x "$DECNETD_BIN" ] || { echo "FATAL: no DECNETD.EXE at $DECNETD_BIN (build it -- see header)" >&2; exit 2; }

# Stage per-run (never a shared path), md5-verified before and after.
LOCAL_MD5=$(md5sum "$DECNETD_BIN" | awk '{print $1}')
kx mkdir -p "$RDIR"
kubectl -n "$NS" cp "$DECNETD_BIN" "$POD:$RDIR/DECNETD.EXE" || { echo "FATAL: cp failed" >&2; exit 2; }
POD_MD5=$(kx md5sum "$RDIR/DECNETD.EXE" 2>/dev/null | awk '{print $1}')
[ "$LOCAL_MD5" = "$POD_MD5" ] || { echo "FATAL: staged md5 mismatch local=$LOCAL_MD5 pod=$POD_MD5" >&2; exit 2; }
kx chmod +x "$RDIR/DECNETD.EXE"
log "staged DECNETD.EXE md5=$LOCAL_MD5 (verified in-pod) at $POD:$RDIR"
log "RUN $TAG pod=$POD iface=$IFACE addr=$ADDR name=$NAME dur=${DUR}s"

# One held-open exec for the whole window (a backgrounded tcpdump does NOT
# survive exec teardown -- lab2run.sh learned this the hard way).
kx sh -c "
  cd $RDIR
  timeout $((DUR+8)) tcpdump -i $IFACE -n -e -tt 'ether proto 0x6003' > $RDIR/$TAG.tcpdump.txt 2>/dev/null &
  TCPD=\$!
  sleep 2
  timeout $((DUR+2)) ./DECNETD.EXE --address $ADDR --name $NAME --iface $IFACE --duration $DUR > $RDIR/$TAG.decnetd.log 2>&1
  wait \$TCPD 2>/dev/null
  true   # tcpdump was killed by its timeout wrapper (expected) -- do not leak 124
"

POST_MD5=$(kx md5sum "$RDIR/DECNETD.EXE" 2>/dev/null | awk '{print $1}')
[ "$LOCAL_MD5" = "$POST_MD5" ] || { echo "FATAL: DECNETD.EXE changed mid-run ($LOCAL_MD5 -> $POST_MD5) -- results UNATTRIBUTABLE" >&2; exit 2; }

echo "===== DECNETD engine log ====="
kx cat "$RDIR/$TAG.decnetd.log"
echo "===== wire (0x6003): per-source endnode-hello src + rtr field ====="
kx sh -c "sed -E 's/.*src (1\\.[0-9]+).*rtr ([0-9.]+).*/\\1 rtr=\\2/; t; d' $RDIR/$TAG.tcpdump.txt | sort | uniq -c"

# Verdict from ground truth: OVMX must have emitted its own hellos AND heard the oracle.
SENT=$(kx grep -c 'HELLOSENT' "$RDIR/$TAG.decnetd.log" 2>/dev/null)
HEARD=$(kx grep -aoE 'heard 1\.[0-9]+' "$RDIR/$TAG.decnetd.log" 2>/dev/null | sort -u | tr '\n' ' ')
OWN=$(kx sh -c "grep -c 'src $ADDR ' $RDIR/$TAG.tcpdump.txt" 2>/dev/null)
echo "===== VERDICT ====="
echo "OVMX hellos on the wire (src $ADDR): $OWN   engine HELLOSENT: $SENT"
echo "OVMX heard oracle node(s): ${HEARD:-<none>}"
if [ "${OWN:-0}" -gt 0 ] && [ -n "$HEARD" ]; then
  echo "PASS(wire-half): OVMX engine ran on the real segment and formed an adjacency entry for a real VAX."
else
  echo "FAIL(wire-half): OVMX did not both emit hellos and hear a real VAX -- check iface/CAP_NET_RAW/segment."
fi
