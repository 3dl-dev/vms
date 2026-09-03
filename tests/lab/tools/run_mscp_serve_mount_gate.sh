#!/bin/bash
# run_mscp_serve_mount_gate.sh - the R4/R5 acceptance harness for FC-P6.3, the
# EXECUTIVE-RESIDENT MSCP disk server: a REAL OpenVMS VAX runs MOUNT against a
# volume a BOOTED OVMX node serves, reads a marker file back, and the whole
# exchange is captured on the wire.
#
# THIS IS NOT A GITHUB-CI GATE and it does not pretend to be one. lab-2 (the
# genuine OpenVMS VAX V7.3 cluster) lives on the k3s cluster and is unreachable
# from GitHub runners; the in-pod boot is TCG-slow. GitHub CI proves the server
# itself at rung R1 -- tests/cluster/host/test_mscp_srv.c, which drives every
# command through the SHIPPING server and runs FC-P3.4's REAL class-driver
# discovery FSM against it -- and this wrapper is the coordinator-run leg that
# only a real VAX can settle. With no opt-in and no lab it SKIPs (exit 77)
# rather than reporting a result it did not obtain.
#
# WHAT ONLY THIS RUN CAN SETTLE (and why R1 green is not enough -- memory
# `h2-green-not-real-vax-join-proven`):
#
#   (a) `%MOUNT-I-MOUNTED` on the VAX console against OVMX's served unit, and a
#       TYPE of a marker file off it. Nothing short of a real class driver
#       accepting OVMX's ONLINE-END, GUS-END and block transfers proves the
#       server is wire-compatible.
#   (b) The BYTE-EXACT ONLINE-END oracle. docs/design-mscp-served-mount-
#       acceptance.md records `mscp-serve$online-end` as honestly `stub`
#       precisely because no capture of an ONLINE-END *accepted by a real VAX*
#       exists. This run is where one comes from.
#   (c) The block-transfer header's `+4`/`+6` words. FC-P6.1 emits an explicit,
#       COUNTED zero for them when the circuit has observed no value
#       (vms_pe_fsm.h SS3b) -- the honest choice, and the one that has never been
#       tested against a real class driver READING our data. If the VAX rejects
#       OVMX's READ payload, this is the first suspect
#       (docs/design-mscp-direction.md: "ungrounded -- do not build on").
#   (d) The UNIT-0 REACHABILITY question test_mscp_srv.c records: FC-P3.4's
#       discovery walk seeds unit word 1, and sec 6.12's MD.NXU is ">=", so a
#       served unit numbered 0 (which is what the executive's own DKA0: yields)
#       is invisible to that walk. What a REAL VMS class driver does here is a
#       measurement, not a thing to decide in code.
#   (e) Whether a real class driver needs the SCC-END controller-flags constant
#       the corpus shows (0xa004) and the reserved 0x0547 beside it. OVMX
#       deliberately does NOT replay either -- both are undecoded, and
#       vms_mscp_srv_fsm.c reports the flags actually in effect instead. If a
#       real driver refuses that answer, THIS is the run that says so.
#
# THE ASYMMETRIC LAB RECIPE (tests/lab/README.md SSMSCP serving, vms-291/vms-3d3).
# Stock lab-2 CANNOT produce serving traffic: both nodes attach the same disk
# images, so neither ever serves to the other. The bed must be made asymmetric
# -- OVMX owns a volume the VAX does not have -- and the VAX must be able to
# name it. On the OVMX side that means MSCP_LOAD=1 and MSCP_SERVE_ALL=1 in
# SYSGEN (which is what vms_mscp_srv_start() gates on) plus a mounted ODS-2
# volume, which is what makes `MSCP$DISK` register at all.
#
# Env:
#   OVMX_LAB2_MSCP   must be "1" or this SKIPs (exit 77).
#   LAB2_POD         target pod (default: vaxlab-0). MUST be a healthy CN_2 pod.
#   ART_DIR          dir with vmlinuz + initramfs-ovmx-slim.cpio.gz +
#                    ovmx-distrib.img (extract from distro/Dockerfile.bootable).
#   SERVE_DUR        window seconds for boot + join + MOUNT (default 600).
#   PCAP_OUT         where to leave the capture (default /tmp/ovmx-mscp-serve.pcap).
#
# Usage:
#   OVMX_LAB2_MSCP=1 LAB2_POD=vaxlab-0 ART_DIR=/tmp/ovmx-boot-art \
#       tests/lab/tools/run_mscp_serve_mount_gate.sh
#
# EXIT: 0 = the VAX mounted and read OVMX's served volume; 1 = it did not
# (an honest RED, which is the instrument working); 77 = not run.
set -uo pipefail
SKIP=77
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "${OVMX_LAB2_MSCP:-0}" != "1" ]; then
    echo "SKIP: the real-VAX-MOUNTs-an-OVMX-served-volume gate needs OVMX_LAB2_MSCP=1"
    echo "      and a live lab-2 pod. GitHub CI proves the server at rung R1"
    echo "      (ctest -R cluster_host_test_mscp_srv: every command through the"
    echo "      shipping server, plus FC-P3.4's real class driver walking it)."
    echo "      This heavy leg is coordinator-run against lab-2 and belongs to"
    echo "      FC-P6.4. See tests/lab/README.md."
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

echo "=== FC-P6.3 R4/R5: a real VAX MOUNTs an OVMX-served volume ==="
echo "pod=$NS/$POD  window=${SERVE_DUR:-600}s  pcap=${PCAP_OUT:-/tmp/ovmx-mscp-serve.pcap}"
echo
echo "STEP 1/4  boot OVMX into lab-2 and reach cluster membership"
echo "STEP 2/4  confirm MSCP\$DISK is REGISTERED on the booted node"
echo "          (it registers only when a serveable unit exists -- so this step"
echo "           is also the proof that a volume is really mounted there)"
echo "STEP 3/4  from the VAX console: MOUNT the served unit, TYPE the marker"
echo "STEP 4/4  verify the pcap carries ONLINE -> ONLINE-END -> GUS -> GUS-END"
echo "          -> READ -> block transfer, ORIGINATING FROM OVMX"
echo

# ---------------------------------------------------------------------------
# THE HARNESS IS WRITTEN; THE RUN IS FC-P6.4's.
#
# Steps 3 and 4 need two things this item does not own: the asymmetric lab bed
# (a volume OVMX has and the VAX does not, tests/lab/README.md SSMSCP serving)
# and the console/pcap acceptance script FC-P6.4 is the plan row for. Rather
# than emit a half-run whose result would mean nothing, this gate stops here
# and says so -- exit 77, the same honest "not run" every other unbuilt lab leg
# in this tree reports. It is deliberately NOT exit 0: a gate that returns
# success without having mounted anything is precisely the fabricated result
# INV-6 exists to prevent.
# ---------------------------------------------------------------------------
echo "SKIP: FC-P6.3 built and proved the SERVER (rung R1). The lab acceptance"
echo "      run -- asymmetric bed + VAX-console MOUNT + pcap grading -- is"
echo "      FC-P6.4's plan row. This harness records what that run must show"
echo "      (steps 1-4 above and the five open questions in this file's header)"
echo "      and refuses to report a result it has not obtained."
exit "$SKIP"
