#!/bin/bash
# run_mscp_write_gate.sh - the R4/R5 acceptance harness for FC-P6.5, the PORT'S
# REQUEST DATA RESPONDER: a WRITE's data really moves, because the SERVER asks
# for it and the host's PORT answers automatically (design
# docs/design-faithful-cluster-executive.md §3.2.6, the E41 ruling).
#
# TWO RUNS, one per direction, and they are different proofs:
#
#   R4  OVMX <-> OVMX. One booted OVMX node serves a volume; another booted
#       OVMX node WRITES a block to it and reads it back. Both ends are this
#       executive, so this is the round trip: client WRITE command -> server
#       REQUEST DATA -> the client PORT's automatic answer -> the server's
#       served-I/O worker commits it -> WRITE end -> the caller completes.
#
#   R5  A REAL VAX WRITES A MARKER onto an OVMX-SERVED volume. This is the one
#       that grades the responder against a real VMS class driver and a real
#       VMS port: OVMX is the SERVER, so OVMX issues the REQUEST DATA and the
#       VAX's port answers it. If OVMX's request is malformed in any field the
#       real port cares about, the marker never lands -- and no amount of
#       host-side testing can substitute for that.
#
# Same posture as every other lab leg in this tree: with no opt-in and no lab
# this SKIPs (exit 77) rather than reporting a result it did not obtain.
#
# WHAT GITHUB CI ALREADY PROVES, so this leg is not load-bearing for the code:
#   ctest -R cluster_host_test_pe_block   -- the responder itself: a REQUEST
#     DATA is answered out of the registered SOURCE buffer, the answer's 28
#     bytes are BYTE-IDENTICAL to the request's (the recorded vms291 WRITE
#     pair), READ's chunking with +8 counting down, and an unknown source
#     buffer is dropped and counted (blk_req_unknown_buffer).
#   ctest -R cluster_host_test_mscp_cl    -- a real WRITE moves real bytes: the
#     server issues the REQUEST DATA, the client's REAL port answers out of the
#     caller's REAL buffer, and the server's fake block device ends up holding
#     the caller's bytes at the caller's LBN.
#   ctest -R cluster_sim_test_sim_mscp_write -- the same exchange between TWO
#     simulated nodes over the virtual LAN, with the disk commit going through
#     the served-I/O worker and NOT the fork thread.
#
# WHAT ONLY THESE RUNS CAN SETTLE:
#
#   (a) THE +4/+6 WORDS ON A REQUEST WE ANSWER. OVMX echoes them verbatim off
#       the request (vms_cluster_codec_blk.h, "THE TWO UNGROUNDED WORDS") and
#       composes neither. Whether a real port accepts an answer that echoes
#       them, and whether it expects anything else in them, is measured here and
#       nowhere else.
#
#   (b) WHETHER A REAL PORT CHUNKS ITS ANSWER THE WAY READ CHUNKS. FC-P6.5 uses
#       READ's chunking (largest chunk first, VMS_BLK_DATA_MAX) because that is
#       the only chunking the capture grounds. A multi-frame WRITE against a
#       real node is what shows whether that is what a real port does.
#
#   (c) WHETHER A REAL SERVER RETRIES A REQUEST DATA WHOSE ANSWER WAS LOST.
#       The port does not retransmit block frames (vms_pe_fsm.h §8d "NO RING");
#       OVMX's recovery is the MSCP deadline, which reaps the command honestly
#       (`writes_undelivered`). What a real server does is unmeasured.
#
#   (d) E48 (raised by FC-P6.5's rung-2 scenario, and a BLOCKER for the R4 run):
#       the port's RECEIVE path delivers a sequenced message to a SYSAP only for
#       length classes the codec grounds a Con.ID for. Of the five MSCP end
#       lengths FC-P6.2 measured, only the 94-content one (WRITE END) is such a
#       class -- so a booted OVMX class driver cannot today receive an SCC,
#       READ, ONLINE or GUS end off the wire. Until that is resolved the R4 run
#       CANNOT reach a WRITE, and this gate says so instead of pretending.
#
# THE LAB BED.
#   R4: two booted OVMX nodes in one cluster, one of them serving a volume the
#       other does not have (the same asymmetry tests/lab/README.md §MSCP
#       serving describes, with OVMX on both ends).
#   R5: the FC-P6.3/P6.4 serving bed -- a real VAX mounting an OVMX-served
#       volume -- plus a WRITE on the VAX (`COPY` a small file onto the served
#       volume, or `$ SET FILE`), and a `DUMP/BLOCK` readback on OVMX.
#
# Env:
#   OVMX_LAB2_MSCP_WRITE  must be "1" or this SKIPs (exit 77).
#   LAB2_POD              target pod (default: vaxlab-0). MUST be a healthy CN_2 pod.
#   ART_DIR               dir with vmlinuz + initramfs-ovmx-slim.cpio.gz +
#                         ovmx-distrib.img (extract from distro/Dockerfile.bootable).
#   WRITE_DUR             window seconds for boot + join + the write (default 600).
#   PCAP_OUT              where to leave the capture
#                         (default /tmp/ovmx-mscp-write.pcap).
#
# Usage:
#   OVMX_LAB2_MSCP_WRITE=1 LAB2_POD=vaxlab-0 ART_DIR=/tmp/ovmx-boot-art \
#       tests/lab/tools/run_mscp_write_gate.sh
#
# EXIT: 0 = the marker bytes really landed on the served volume and the pcap
# carries the REQUEST DATA / answer pair; 1 = they did not (an honest RED, which
# is the instrument working); 77 = not run.
set -uo pipefail
SKIP=77
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "${OVMX_LAB2_MSCP_WRITE:-0}" != "1" ]; then
    echo "SKIP: the MSCP WRITE / REQUEST DATA gate needs OVMX_LAB2_MSCP_WRITE=1"
    echo "      and a live lab-2 pod. GitHub CI proves the responder at rung R1"
    echo "      (cluster_host_test_pe_block, cluster_host_test_mscp_cl) and at"
    echo "      rung R2 (cluster_sim_test_sim_mscp_write). This heavy leg is"
    echo "      coordinator-run against lab-2."
    exit "$SKIP"
fi

command -v kubectl >/dev/null 2>&1 || {
    echo "SKIP: kubectl not available -- no lab-2 access"; exit "$SKIP"; }

POD="${LAB2_POD:-vaxlab-0}"
NS="${NS:-ovmx-lab}"
kubectl -n "$NS" get pod "$POD" >/dev/null 2>&1 || {
    echo "SKIP: pod $NS/$POD not reachable"; exit "$SKIP"; }

ART_DIR="${ART_DIR:?ART_DIR required: dir with vmlinuz, initramfs-ovmx-slim.cpio.gz, ovmx-distrib.img}"
for f in vmlinuz initramfs-ovmx-slim.cpio.gz ovmx-distrib.img; do
    [ -f "$ART_DIR/$f" ] || {
        echo "FATAL: $ART_DIR/$f missing (extract from distro/Dockerfile.bootable)"
        exit 1; }
done

# The boot/join half is EXACTLY the FC-P3.9 acceptance harness's -- there is no
# second way to get a booted OVMX node into lab-2, and a copy of that logic here
# would be a second thing to keep correct.
JOIN_GATE="$HERE/run_labjoin_booted_gate.sh"
[ -x "$JOIN_GATE" ] || {
    echo "FATAL: $JOIN_GATE missing -- the booted-join harness is this gate's first half"
    exit 1; }

echo "=== FC-P6.5 R4/R5: a WRITE's data really moves (REQUEST DATA + responder) ==="
echo "pod=$NS/$POD  window=${WRITE_DUR:-600}s  pcap=${PCAP_OUT:-/tmp/ovmx-mscp-write.pcap}"
echo
echo "R4  STEP 1/4  boot two OVMX nodes into lab-2 and reach membership"
echo "R4  STEP 2/4  one serves a volume the other does not have; the other's"
echo "              class driver enumerates it and brings the unit ONLINE"
echo "R4  STEP 3/4  WRITE a marker block; the pcap must carry, in order:"
echo "              WRITE command -> header-only block frame (REQUEST DATA)"
echo "              -> the SAME 28 bytes WITH data (the port's answer)"
echo "              -> WRITE END with P.BCNT == the bytes written"
echo "R4  STEP 4/4  READ it back on the writer and byte-compare the marker"
echo
echo "R5  STEP 1/3  a real VAX mounts the OVMX-served volume (the FC-P6.3 bed)"
echo "R5  STEP 2/3  on the VAX, write a marker file onto that volume; the pcap"
echo "              must carry OVMX's REQUEST DATA and the VAX PORT's answer"
echo "R5  STEP 3/3  on OVMX, DUMP/BLOCK the backing device and find the marker"
echo

# ---------------------------------------------------------------------------
# THE HARNESS IS WRITTEN; THE RUNS NEED A BED THIS ITEM DOES NOT OWN -- AND R4
# IS ALSO BLOCKED ON E48 (see this file's header, ask (d)): a booted OVMX class
# driver cannot yet RECEIVE the SCC/GUS/ONLINE end messages off the wire, so it
# cannot reach a WRITE at all. Emitting a half-run whose result would mean
# nothing is worse than saying so.
#
# Exit 77, the same honest "not run" every other unbuilt lab leg in this tree
# reports. It is deliberately NOT exit 0 -- a gate that returns success without
# having seen a marker land is precisely the fabricated result INV-6 exists to
# prevent.
# ---------------------------------------------------------------------------
echo "SKIP: FC-P6.5 built and proved the RESPONDER at rungs R1 and R2. The lab"
echo "      acceptance runs -- the two-OVMX bed for R4, the VAX-writes-a-marker"
echo "      bed for R5, and the pcap grading of the REQUEST DATA / answer pair --"
echo "      need a bed this item does not own, and R4 additionally needs E48"
echo "      resolved. This harness records what those runs must show (the steps"
echo "      above and the four open questions in this file's header) and refuses"
echo "      to report a result it has not obtained."
exit "$SKIP"
