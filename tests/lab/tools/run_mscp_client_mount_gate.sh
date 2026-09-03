#!/bin/bash
# run_mscp_client_mount_gate.sh - the R4 acceptance harness for FC-P7.1, the
# EXECUTIVE-RESIDENT MSCP DISK CLASS DRIVER: a BOOTED OVMX node discovers a
# volume ANOTHER cluster member serves, that unit appears as a real device in
# the executive's device table, and the ACP reads a block off it -- over the
# wire, from a disk that is not on this node.
#
# It is the MIRROR of run_mscp_serve_mount_gate.sh (FC-P6.3/P6.4's leg, which
# runs the traffic the other way: a real VAX mounting an OVMX-served volume).
# Same posture, same honesty rule: with no opt-in and no lab this SKIPs
# (exit 77) rather than reporting a result it did not obtain.
#
# WHAT GITHUB CI ALREADY PROVES, so this leg is not load-bearing for the code:
#   ctest -R cluster_host_test_mscp_cl -- FC-P7.1's REAL class driver against
#   FC-P6.3's REAL server and the client's own REAL port, in one host process:
#   the SCC x2 + GUS walk, served units becoming named devices, ONLINE, a READ
#   whose bytes are asserted to be the server's own block-device bytes (moved by
#   real named-buffer transfers, with the final chunk arriving piggybacked on
#   the end message), the short-read refusal, and the controller timeout.
#
# WHAT ONLY THIS RUN CAN SETTLE (memory `h2-green-not-real-vax-join-proven`):
#
#   (a) THE ALLOCATION CLASS. FC-P7.1 names a served unit `<SCSNODE>$DUAn:`
#       because this executive has NO grounded transport for the serving node's
#       ALLOCLASS -- not the CSB, not the cat-0x01 PARAMS record, not MSCP
#       itself (vms_mscp_cl_io_fsm.h, "THE SERVED DEVICE'S NAME"). Design P7
#       spells the device `$2$DUA0:`. What a REAL VMS class driver learns the
#       serving node's allocation class FROM is a MEASUREMENT this run must
#       take, not a thing to decide in code. Until it is taken, OVMX names what
#       it can defend and counts the omission (`alloclass_absent`).
#
#   (b) UNIT 0. FC-P3.4's walk seeds unit word 1 and sec 6.12's MD.NXU is ">=",
#       so a served unit numbered 0 is invisible to it -- the same open fact
#       test_mscp_srv.c records from the SERVER's side (integration note E39
#       ask 1). Whether a real class driver reaches a unit 0, and with what
#       seed, is settled here.
#
#   (c) WRITE's BLOCK-TRANSFER INITIATION DIRECTION (E39 ask 2). The capture
#       grounds only that WRITE's two 28-byte headers are BYTE-IDENTICAL; it
#       does NOT say which side sends the first of the two. FC-P7.1 therefore
#       issues a real WRITE command with a real named buffer and then WAITS,
#       and its own deadline reaps the request honestly -- it invents no
#       initiation (`writes_undelivered` counts exactly that outcome). A capture
#       of a real VMS class driver WRITING to a served volume is what closes it.
#
#   (d) THE BLOCK HEADER'S +4/+6 WORDS, from the RECEIVE side. OVMX now LEARNS
#       them off a frame that really arrived (vms_pe_fsm.h SS3b) and echoes them
#       back. Against a real server that is untested.
#
#   (e) Whether a real server's READ answer really piggybacks its final chunk on
#       the end message the way the vms291 capture recorded, on the connection
#       OVMX itself opened. FC-P7.1 landed TRAP 1's RECEIVE arm
#       (pe_blk_rx_trailer_try) for exactly that shape; if a real server streams
#       the whole transfer standalone instead, this run is where that shows.
#
# THE LAB BED. The mirror of the serving recipe (tests/lab/README.md SSMSCP
# serving, vms-291/vms-3d3): the bed must be ASYMMETRIC THE OTHER WAY -- the
# VAX owns a volume OVMX does not have -- and the VAX must be serving it
# (MSCP_LOAD=1 / MSCP_SERVE_ALL=1 in ITS SYSGEN, plus the volume mounted there).
# Stock lab-2 cannot produce this traffic either: both nodes attach the same
# disk images, so neither has anything the other needs.
#
# Env:
#   OVMX_LAB2_MSCP_CL  must be "1" or this SKIPs (exit 77).
#   LAB2_POD           target pod (default: vaxlab-0). MUST be a healthy CN_2 pod.
#   ART_DIR            dir with vmlinuz + initramfs-ovmx-slim.cpio.gz +
#                      ovmx-distrib.img (extract from distro/Dockerfile.bootable).
#   CLIENT_DUR         window seconds for boot + join + discovery (default 600).
#   PCAP_OUT           where to leave the capture
#                      (default /tmp/ovmx-mscp-client.pcap).
#
# Usage:
#   OVMX_LAB2_MSCP_CL=1 LAB2_POD=vaxlab-0 ART_DIR=/tmp/ovmx-boot-art \
#       tests/lab/tools/run_mscp_client_mount_gate.sh
#
# EXIT: 0 = OVMX discovered, named and READ a VAX-served unit; 1 = it did not
# (an honest RED, which is the instrument working); 77 = not run.
set -uo pipefail
SKIP=77
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "${OVMX_LAB2_MSCP_CL:-0}" != "1" ]; then
    echo "SKIP: the OVMX-mounts-a-VAX-served-disk gate needs OVMX_LAB2_MSCP_CL=1"
    echo "      and a live lab-2 pod. GitHub CI proves the class driver at rung"
    echo "      R1 (ctest -R cluster_host_test_mscp_cl: FC-P7.1's real driver"
    echo "      against FC-P6.3's real server over the client's real port)."
    echo "      This heavy leg is coordinator-run against lab-2."
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

echo "=== FC-P7.1 R4: OVMX discovers and reads a VAX-SERVED volume ==="
echo "pod=$NS/$POD  window=${CLIENT_DUR:-600}s  pcap=${PCAP_OUT:-/tmp/ovmx-mscp-client.pcap}"
echo
echo "STEP 1/5  boot OVMX into lab-2 and reach cluster membership"
echo "STEP 2/5  confirm the VAX is SERVING a volume OVMX does not have"
echo "          (MSCP_LOAD/MSCP_SERVE_ALL on the VAX + that volume mounted there)"
echo "STEP 3/5  on OVMX: the class driver's own MSCP\$DISK connection opens and"
echo "          its SCC x2 + GUS walk enumerates the VAX's units"
echo "STEP 4/5  SHOW DEVICE on OVMX lists the served unit, and \$GETDVI reports"
echo "          DVI\$_MSCP_SERVED = 1 for it -- read off the real device row"
echo "STEP 5/5  read a block off it and verify the pcap carries"
echo "          ONLINE -> ONLINE-END -> READ -> block transfer, INBOUND to OVMX"
echo

# ---------------------------------------------------------------------------
# THE HARNESS IS WRITTEN; THE RUN NEEDS A BED THIS ITEM DOES NOT OWN.
#
# Steps 2-5 need the ASYMMETRIC lab bed described in this file's header -- a
# volume the VAX has and OVMX does not, with the VAX configured to serve it --
# plus the console/pcap acceptance script. Rather than emit a half-run whose
# result would mean nothing, this gate stops here and says so: exit 77, the same
# honest "not run" every other unbuilt lab leg in this tree reports. It is
# deliberately NOT exit 0 -- a gate that returns success without having read a
# served block is precisely the fabricated result INV-6 exists to prevent.
# ---------------------------------------------------------------------------
echo "SKIP: FC-P7.1 built and proved the CLASS DRIVER (rung R1). The lab"
echo "      acceptance run -- asymmetric bed + SHOW DEVICE/\$GETDVI readback +"
echo "      pcap grading -- needs a lab bed this item does not own. This harness"
echo "      records what that run must show (steps 1-5 above and the five open"
echo "      questions in this file's header) and refuses to report a result it"
echo "      has not obtained."
exit "$SKIP"
