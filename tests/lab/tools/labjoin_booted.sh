#!/bin/bash
# labjoin_booted.sh <pod> <tag> <artifacts-dir> <duration> [SCSNODE SCSSYSTEMID]
#
# THE REAL 0.6 CLUSTER GATE (rd vms-fa1a -> enables vms-5a23, CLOSES vms-110b).
# Prove that a BOOTED OVMX node -- the shipped distro/Dockerfile.bootable image,
# booting through STARTUP to DCL and auto-starting SCS (vms-5ad/110b.1) -- joins a
# REAL VAX VMScluster: lab-2 (a genuine OpenVMS VAX V7.3 2-node cluster in a k3s
# pod, the project's clean-room oracle). PASS means all four legs (see
# labjoin_lib.sh lj_verdict): OVMX sees a VAX, the VAX sees OVMX, CLUSTER_NODES
# grows to 3, and the join is captured on the wire.
#
# WHY THIS IS NOT lab2run.sh. lab2run runs a BARE SCSD.EXE probe (a Linux ELF)
# inside the pod. This runs the BOOTED RUNTIME instead: a full OVMX image under
# QEMU/TCG inside the pod, tap-bridged to the pod's br0 -- the same L2 placement
# the probe used, but the actual shipping node, which is the milestone bar the
# probe never met (memory: "a booted node is not a cluster member" -- this is the
# harness that will prove it becomes one).
#
# EXPECTED STATE: RED until vms-5ad lands. A booted OVMX today STAGES SCSD.EXE but
# never spawns it (ovmx_init.c:953), so it never joins -- SHOW CLUSTER shows no
# VAX and this gate FAILS honestly. That is correct: this is the anti-fabrication
# instrument. It goes GREEN when 110b.1 makes the booted node auto-start SCS. It
# is NOT stubbed green and it does NOT touch the guest's scsd/ovmx_init/VMS$VMS.DAT.
#
# ⚠ RESOURCE: lab-2 is precious and the in-pod TCG boot is heavy. Check the pod is
# a healthy 2-node cluster (CN_2) BEFORE joining (done here), never point two runs
# at one pod, and coordinate the heavy run -- see tests/lab/README.md.
#
# Env (defaults match lab2run.sh; overridable for the mocked plumbing test):
#   NS      k8s namespace                        (ovmx-lab)
#   L       pod-side log dir                     (/lab/k8s-labs/<pod>/logs)
#   HOSTL   host-readable log dir (tank)         (/data/training/vax/k8s-labs/<pod>/logs)
#   W       identity registry / work dir         (/data/training/vax/cluster/work)
#   MK      mk_sysgen.py path                    (<repo>/tests/lab/tools/mk_sysgen.py)
#   DUR_POLL join poll iterations (x ~15s)       (40 -- TCG boot is slow)
set -u

POD="${1:?usage: labjoin_booted.sh <pod> <tag> <artifacts-dir> <duration> [SCSNODE SCSSYSTEMID]}"
TAG="${2:?tag required (unique per run)}"
ART="${3:?artifacts-dir required (vmlinuz, initramfs-ovmx-slim.cpio.gz, ovmx-distrib.img)}"
DUR="${4:?duration seconds required}"
REQ_NODE="${5:-}"
REQ_SYSID="${6:-}"

NS="${NS:-ovmx-lab}"
L="${L:-/lab/k8s-labs/$POD/logs}"
HOSTL="${HOSTL:-/data/training/vax/k8s-labs/$POD/logs}"
W="${W:-/data/training/vax/cluster/work}"
REPO="${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}"
MK="${MK:-$REPO/tests/lab/tools/mk_sysgen.py}"
DUR_POLL="${DUR_POLL:-40}"
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=labjoin_lib.sh
. "$HERE/labjoin_lib.sh"

kx()  { kubectl -n "$NS" exec "$POD" -- "$@"; }
# Drive vax1's console via its input FIFO; read its console log from the volume.
vsay() { kubectl -n "$NS" exec "$POD" -- sh -c "printf '%s\\r' '$1' > $L/vax1.log.in"; sleep "${2:-2}"; }
vclean() { lj_clean "$HOSTL/vax1.log"; }
log() { echo "[$(date +%T)] $*"; }

kubectl -n "$NS" get pod "$POD" >/dev/null 2>&1 || { echo "labjoin: FATAL -- no pod $POD in ns $NS" >&2; exit 2; }
[ -d "$ART" ] || { echo "labjoin: FATAL -- artifacts dir $ART missing" >&2; exit 2; }

log "RUN $TAG pod=$POD artifacts=$ART dur=$DUR"

# --- 1. Mint / validate a collision-free identity ---------------------------
if [ -n "$REQ_NODE" ] && [ -n "$REQ_SYSID" ]; then
    SCSNODE="$REQ_NODE"; SCSSYSID="$REQ_SYSID"
    log "using caller-supplied identity SCSNODE=$SCSNODE SCSSYSTEMID=$SCSSYSID"
else
    ALLOC="$(python3 "$MK" --alloc "${OVMX_PREFIX:-OVMXJ}" "$W" 2>/dev/null)" \
        || { echo "labjoin: FATAL -- mk_sysgen --alloc failed against $W" >&2; exit 2; }
    SCSNODE="$(echo "$ALLOC" | awk '{print $1}')"
    SCSSYSID="$(echo "$ALLOC" | awk '{print $2}')"
    log "minted identity SCSNODE=$SCSNODE SCSSYSTEMID=$SCSSYSID (mk_sysgen --alloc)"
fi
lj_guard_identity "$SCSSYSID" || exit 2   # refuse the pod's reserved VAX ids

# --- 2. CN_2 precheck: the pod MUST be a healthy 2-node cluster -------------
# (tests/lab/README.md "Check the pod is a CLUSTER": a Running pod can be CN_1,
# and a join test against it fails for reasons unrelated to the node under test.)
vsay "WRITE SYS\$OUTPUT \"CN_\"+F\$STRING(F\$GETSYI(\"CLUSTER_NODES\"))" 4
CN0="$(vclean | lj_parse_cn)"
if [ "$CN0" != "2" ]; then
    echo "labjoin: FATAL -- pod $POD is not a healthy 2-node cluster (CLUSTER_NODES=${CN0:-?}, want 2)." >&2
    echo "  A join against a CN_1/CN_3 pod is unattributable. Scale a fresh replica:" >&2
    echo "    kubectl -n $NS scale sts/vaxlab --replicas=N   # gives a virgin CN_2 pod" >&2
    exit 2
fi
log "precheck OK: pod is CN_2 (healthy 2-node VAX cluster)"

# --- 3. Create the OVMX node's tap on the pod's br0 -------------------------
# vax1/vax2/vax3 own tap1/tap2/tap3 (entrypoint.sh node_tap); the OVMX node takes
# tap4. Idempotent -- a re-run reuses it.
OVMX_TAP="${OVMX_TAP:-tap4}"
kx sh -c "ip tuntap add dev $OVMX_TAP mode tap 2>/dev/null; ip link set $OVMX_TAP master br0; ip link set $OVMX_TAP up" \
    || { echo "labjoin: FATAL -- could not create $OVMX_TAP on br0 in $POD" >&2; exit 2; }
log "tap $OVMX_TAP up on br0"

# --- 4. Stage artifacts + harness scripts into a per-run pod dir ------------
# Per-run dir (never a shared path) + md5-verify the kernel in-pod, exactly as
# lab2run.sh does for SCSD.EXE, so a concurrent session or a truncated copy is
# caught, not silently trusted.
RDIR="/lab/run-$TAG"
kx mkdir -p "$RDIR"
stage() {  # <local> <podname>
    local src="$1" name="$2"
    [ -f "$src" ] || { echo "labjoin: FATAL -- missing $src to stage" >&2; exit 2; }
    kubectl -n "$NS" cp "$src" "$POD:$RDIR/$name" \
        || { echo "labjoin: FATAL -- kubectl cp $src -> $POD:$RDIR/$name failed" >&2; exit 2; }
}
stage "$ART/vmlinuz" vmlinuz
stage "$ART/initramfs-ovmx-slim.cpio.gz" initramfs-ovmx-slim.cpio.gz
stage "$ART/ovmx-distrib.img" ovmx-distrib.img
stage "$HERE/labjoin_lib.sh" labjoin_lib.sh
stage "$HERE/labjoin_pod_boot.sh" labjoin_pod_boot.sh
K_MD5="$(md5sum "$ART/vmlinuz" | awk '{print $1}')"
P_MD5="$(kx md5sum "$RDIR/vmlinuz" 2>/dev/null | awk '{print $1}')"
[ "$K_MD5" = "$P_MD5" ] || { echo "labjoin: FATAL -- staged vmlinuz md5 mismatch (local=$K_MD5 pod=$P_MD5)" >&2; exit 2; }
kx chmod +x "$RDIR/labjoin_pod_boot.sh" "$RDIR/labjoin_lib.sh"
log "staged artifacts + drivers at $RDIR (vmlinuz md5=$K_MD5, verified in-pod)"

# --- 5. Capture the join on the wire (0x6007 SCA), for the whole run --------
# ⚠ A backgrounded process does not survive kubectl-exec teardown -- HOLD THE
# SESSION OPEN: foreground exec, backgrounded on the HOST side (lab2run.sh).
NODE_LOG="$L/ovmx-node-$TAG.log"              # pod path
HOST_NODE_LOG="$HOSTL/ovmx-node-$TAG.log"     # tank path (host-readable)
CAP_EVID="$L/ovmx-node-$TAG.caps"            # pod path: CAP_NET_RAW evidence (leg e)
HOST_CAP_EVID="$HOSTL/ovmx-node-$TAG.caps"   # tank path (host-readable)
PCAP="$L/join-$TAG.pcap"
HOST_PCAP="$HOSTL/join-$TAG.pcap"
kubectl -n "$NS" exec "$POD" -- timeout $((DUR + 120)) \
    tcpdump -i br0 -w "$PCAP" -U -s 0 'ether proto 0x6007' >/dev/null 2>&1 &
TCPD=$!
sleep 2

# --- 6. Boot + drive the OVMX node inside the pod (foreground, host-bg) -----
# OVMX_DROP_NET_RAW=1: run the booted node with CAP_NET_RAW dropped from its
# whole process subtree (anti-fabrication teeth, leg (e)) -- a join under this is
# proof the executive did the L2 I/O, not the pod's ambient cap. CAP_EVID is where
# labjoin_pod_boot.sh records the actual capability set for the verdict to grade.
kubectl -n "$NS" exec "$POD" -- sh -c \
    "cd $RDIR && ART_DIR=$RDIR OUT_LOG=$NODE_LOG CAP_EVID=$CAP_EVID OVMX_DROP_NET_RAW=1 SCSNODE=$SCSNODE SCSSYSID=$SCSSYSID OVMX_TAP=$OVMX_TAP JOIN_POLL=$DUR BOOT_TO=$((DUR + 120)) ./labjoin_pod_boot.sh" \
    >/dev/null 2>&1 &
NODEP=$!
log "OVMX node booting in $POD (exec sessions held open: tcpdump=$TCPD node=$NODEP)"

# --- 7. Poll the VAX oracle THROUGH the window for a REAL MEMBER admission --
# Do NOT stop at CLUSTER_NODES=3. F$GETSYI("CLUSTER_NODES") counts a BRK_NON CSB
# -- a node the VAX has HEARD (created a CSB for) but has NOT admitted -- so CN=3
# alone is not admission (lab-2 vms-a84d: OVMXJ0 appeared in CN=3 while the VAX
# held it BRK_NON, and OVMX self-rendered MEMBER ahead of the VAX's real
# promotion). The authentic signal is the VAX's OWN SHOW CLUSTER listing OVMXJ0
# with STATUS == MEMBER. Each iteration, capture vax1 SHOW CLUSTER + OVMXJ0's
# status; keep the STRONGEST snapshot seen (a MEMBER one if it ever appears, else
# the latest that names OVMXJ0). Keep polling through the window so the node stays
# LIVE for an independent VAX-side eye -- never tear down on CN=3 or an OVMX self-
# report; only the node exiting on its own ends the poll early.
joined=0
VAX_SC=""
NODE_UP="$(printf '%s' "$SCSNODE" | tr '[:lower:]' '[:upper:]' | cut -c1-6)"
for i in $(seq 1 "$DUR_POLL"); do
    sleep 15
    vsay "WRITE SYS\$OUTPUT \"CN_\"+F\$STRING(F\$GETSYI(\"CLUSTER_NODES\"))" 3
    n="$(vclean | lj_parse_cn)"
    vsay 'SHOW CLUSTER' 6
    sc="$(vclean)"
    st="$(printf '%s\n' "$sc" | grep -aiE "$NODE_UP" | grep -aoiE 'MEMBER|BRK_[A-Z]+|NEW|OPEN[A-Z]*' | head -1)"
    log "  t+$((i*15))s CLUSTER_NODES=${n:-?}  ${NODE_UP}-status=${st:-absent}"
    if printf '%s\n' "$sc" | grep -aiE "$NODE_UP" | grep -qaiE 'MEMBER'; then
        VAX_SC="$sc"; [ "$joined" = 0 ] && log "  VAX ORACLE ADMITS $NODE_UP AS MEMBER (authentic admission)"; joined=1
    elif [ -z "$VAX_SC" ] || printf '%s\n' "$sc" | grep -qaiE "$NODE_UP"; then
        VAX_SC="$sc"   # remember the latest snapshot that at least names OVMXJ0
    fi
    kill -0 "$NODEP" 2>/dev/null || { log "OVMX node driver exited"; break; }
done

# --- 8. Final authoritative snapshot of vax1's SHOW CLUSTER (keep the best) --
vsay 'SHOW CLUSTER' 6
last_sc="$(vclean)"
if printf '%s\n' "$last_sc" | grep -aiE "$NODE_UP" | grep -qaiE 'MEMBER'; then VAX_SC="$last_sc"; fi
[ -z "$VAX_SC" ] && VAX_SC="$last_sc"

# Wind down the node + capture.
kx pkill -f qemu-system 2>/dev/null
wait "$NODEP" 2>/dev/null
wait "$TCPD" 2>/dev/null

# --- 9. Verdict: read every transcript from the tank volume, grade all four --
# join legs AND the cap-denied teeth (leg e) -- the gate is the AND of both.
OVMX_SC="$(lj_clean "$HOST_NODE_LOG" 2>/dev/null)"
CN_FINAL="$(vclean | lj_parse_cn)"
[ -z "$CN_FINAL" ] && CN_FINAL="$CN0"
CAP_EVIDENCE="$(cat "$HOST_CAP_EVID" 2>/dev/null)"

echo ""
echo "=========================================="
echo "  identity on the wire: $(strings -a "$HOST_PCAP" 2>/dev/null | grep -oE 'OVMX[A-Z0-9]{2,}' | sort -u | tr '\n' ' ')"
echo "  artifacts: node=$HOST_NODE_LOG  pcap=$HOST_PCAP  caps=$HOST_CAP_EVID  vax1=$HOSTL/vax1.log"
lj_booted_gate_verdict "$SCSNODE" "$OVMX_SC" "$VAX_SC" "$CN_FINAL" "$HOST_PCAP" "$CAP_EVIDENCE"
V=$?
echo "=========================================="
exit "$V"
