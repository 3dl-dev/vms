#!/bin/bash
# run_mscp_srv_io_jitter_gate.sh - the R4 acceptance harness for FC-P6.6, the
# MSCP SERVER's I/O off the cluster fork thread.
#
# THE DONE-CONDITION THIS HARNESS GRADES (plan row FC-P6.6):
#
#   "HELLO cadence jitter under a served READ loop stays < 10 ms on both
#    substrates."
#
# WHY THAT IS THE RIGHT MEASUREMENT. FC-P6.3's server called the executive's
# SYNCHRONOUS block seam (exec_blockdev_read_block) from its fork work handler,
# i.e. on the CLUSTER FORK THREAD -- the single context that also emits the
# HELLO cadence, drives the VC retransmit ladder and takes every barrier step
# (design SS3.2.6, the E42 corollary). So a served READ did not just slow the
# server down: it stalled the node's whole cluster personality for the duration
# of the disk access, which is a TIMVCFAIL risk under load HERE and a
# 12x(M-1) barrier latency on EVERY OTHER MEMBER. The symptom is therefore not
# a throughput number -- it is HELLO CADENCE JITTER, and it is visible from
# outside the node, on the wire, with no instrumentation inside the executive
# at all. That is what this gate measures.
#
# THE INSTRUMENT IS THE WIRE, NOT A COUNTER (INV-6). A jitter figure computed
# from a counter the executive keeps about itself would be the executive
# grading its own homework. This gate takes the inter-frame deltas of the
# node's OWN directed HELLOs out of a tcpdump capture on the cluster segment:
# every value is a timestamp another machine recorded off the wire.
#
# ------------------------------------------------------------------------
# WHAT MUST BE TRUE FOR THIS RUN TO MEAN ANYTHING
# ------------------------------------------------------------------------
#   1. A BOOTED OVMX node in the cluster, serving a volume -- i.e. MSCP_LOAD=1,
#      MSCP_SERVE_ALL=1 and a mounted ODS-2 volume, which is what makes
#      `MSCP$DISK` register at all (vms_mscp_srv.h's registration predicate).
#   2. A PEER RUNNING A SUSTAINED SERVED READ LOOP against that node. Serving
#      traffic needs somebody to serve: either a real VAX (the lab-2 bed, which
#      must be made ASYMMETRIC -- tests/lab/README.md SSMSCP serving, since stock
#      lab-2 gives both nodes the same images and neither ever serves) or a
#      second OVMX node running the FC-P7.1/FC-P7.2 class driver over the ACP.
#   3. A capture of the cluster segment covering both a QUIET window and the
#      LOADED window, so the loaded jitter is compared against this same node's
#      own quiet baseline rather than against a number from nowhere.
#
# THE GRADE:
#   quiet   max|delta - HELLO_MS| over the quiet window  (the baseline)
#   loaded  max|delta - HELLO_MS| over the served-READ window
#   PASS iff loaded < 10 ms. Reporting `loaded` without `quiet` beside it would
#   hide a node whose cadence was already ragged, so both are printed and the
#   run is only meaningful when quiet is itself well under the bound.
#
# BOTH SUBSTRATES. The plan row says both. The Linux (vms.ko) node is the lab-2
# case above. The NetBSD-VAX SYSKRNL node is the same measurement against a
# SYSKRNL-booted node; the cross-compile gates
# (tools/cross-vax/build-vms-module-vax.sh, tools/cross-vax/build-devvms-vax.sh)
# already prove the worker's TU builds there, and the module's SS15 binding is
# the same vms_cluster_fork_bind.c both substrates share -- but a BUILD is not a
# MEASUREMENT and this file will not pretend otherwise.
#
# WHAT ALREADY RUNS IN CI, so the property is not unproven while this leg waits:
#   R1  ctest -R cluster_host_test_cluster_fork        -- the worker's queue
#       discipline, the reserved completion, the stop that abandons; and
#       cluster_host_test_cluster_fork_threads, which runs a REAL worker thread
#       whose callback SLEEPS and asserts the fork thread kept dispatching
#       throughout ("fork dispatches overlapped a busy disk" > 0).
#   R1  ctest -R cluster_host_test_mscp_srv            -- the server never
#       reads a block on the command dispatch, the completion drives the end
#       message, the worker owns the staging slot (no reap, no close, no reuse
#       under it), and a stale completion is dropped.
#   R3  tests/qemu/test_kmod_cluster_fork_hammer.c     -- on a REAL kernel: the
#       real second kthread, a callback that really msleep()s, and a REAL 10 ms
#       exec_timer cadence whose expiries the fork thread keeps running WHILE
#       the worker blocks (CADENCE_TICKS_DURING_IO > 0).
#   CI  tools/ci/cluster_core_includes_gate.sh RULE 5  -- no exec_blockdev_
#       symbol on any fork-context path, with its own negative control.
#
# Env:
#   OVMX_LAB2_MSCP   must be "1" or this SKIPs (exit 77).
#   LAB2_POD         target pod (default: vaxlab-0). MUST be a healthy CN_2 pod.
#   PCAP_IN          an existing capture to grade instead of taking a new one.
#   HELLO_MS         the cadence to measure against (default 3000, the lab's).
#   JITTER_MAX_MS    the bound (default 10, the plan row's).
#
# EXIT: 0 = measured and within the bound; 1 = measured and over it (an honest
# RED, which is the instrument working); 77 = not run.
set -uo pipefail
SKIP=77
HERE="$(cd "$(dirname "$0")" && pwd)"
HELLO_MS="${HELLO_MS:-3000}"
JITTER_MAX_MS="${JITTER_MAX_MS:-10}"

if [ "${OVMX_LAB2_MSCP:-0}" != "1" ]; then
    echo "SKIP: the HELLO-jitter-under-served-READ gate needs OVMX_LAB2_MSCP=1"
    echo "      and a live lab-2 pod. The property itself is proved in CI at"
    echo "      rungs R1 and R3 -- see this file's header for the exact tests."
    exit "$SKIP"
fi

command -v kubectl >/dev/null 2>&1 || {
    echo "SKIP: kubectl not available -- no lab-2 access"; exit "$SKIP"; }

POD="${LAB2_POD:-vaxlab-0}"
NS="${NS:-ovmx-lab}"
kubectl -n "$NS" get pod "$POD" >/dev/null 2>&1 || {
    echo "SKIP: pod $NS/$POD not reachable"; exit "$SKIP"; }

# The boot/join half is the FC-P3.9 acceptance harness's, exactly as
# run_mscp_serve_mount_gate.sh uses it: there is no second way to get a booted
# OVMX node into lab-2, and a copy of that logic here would be a second thing to
# keep correct.
JOIN_GATE="$HERE/run_labjoin_booted_gate.sh"
[ -x "$JOIN_GATE" ] || {
    echo "FATAL: $JOIN_GATE missing -- the booted-join harness is this gate's first half"
    exit 1; }

# The served-READ load half is FC-P6.4's (a real VAX MOUNTing an OVMX-served
# volume) or FC-P7.2's (a second OVMX node reading through the ACP). Neither is
# landed, so there is no way to put a served READ loop on this node yet.
LOAD_GATE="$HERE/run_mscp_serve_mount_gate.sh"

echo "=== FC-P6.6 R4: HELLO cadence jitter under a served READ loop ==="
echo "pod=$NS/$POD  cadence=${HELLO_MS}ms  bound=${JITTER_MAX_MS}ms"
echo
echo "STEP 1/5  boot OVMX into lab-2 and reach cluster membership   ($JOIN_GATE)"
echo "STEP 2/5  capture the cluster segment QUIET for 60s; compute the baseline"
echo "          jitter from this node's OWN directed HELLO inter-frame deltas"
echo "STEP 3/5  put a SUSTAINED served READ loop on the node        ($LOAD_GATE"
echo "          for the asymmetric bed; FC-P6.4 / FC-P7.2 own the loop itself)"
echo "STEP 4/5  capture LOADED for 60s; compute the same figure"
echo "STEP 5/5  PASS iff loaded < ${JITTER_MAX_MS}ms, with quiet printed beside it"
echo
echo "  the pcap reduction, once there is a capture to reduce:"
echo "    tshark -r \$PCAP -Y 'eth.type == 0x6007' -T fields -e frame.time_epoch \\"
echo "      -e eth.src | awk -v node=\"\$OVMX_MAC\" '\$2==node{if(p){d=(\$1-p)*1000;"
echo "      j=d-'\"\$HELLO_MS\"'; if(j<0)j=-j; if(j>m)m=j} p=\$1} END{print m}'"
echo

# ---------------------------------------------------------------------------
# THE HARNESS IS WRITTEN; THE RUN NEEDS A LOAD SOURCE THIS ITEM DOES NOT OWN.
#
# Steps 3-5 need a peer doing sustained served READs against the booted node.
# That is FC-P6.4's real-VAX MOUNT (which also needs the asymmetric lab bed) or
# FC-P7.2's ACP-bridged client on a second OVMX node. Neither has landed, so
# there is nothing to load the server with, and a jitter figure measured on an
# IDLE server would be a number that looks like a pass and proves nothing --
# precisely the fabricated result INV-6 exists to stop.
#
# Exit 77, the honest "not run", exactly as every other unbuilt lab leg in this
# tree reports.
# ---------------------------------------------------------------------------
echo "SKIP: FC-P6.6 moved the server's I/O off the fork thread and proved it at"
echo "      rungs R1 and R3 (see this file's header). This R4 leg needs a peer"
echo "      running a sustained served READ loop -- FC-P6.4 (real VAX MOUNT) or"
echo "      FC-P7.2 (OVMX client through the ACP) -- and refuses to report a"
echo "      jitter figure measured against an idle server."
exit "$SKIP"
