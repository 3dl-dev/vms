#!/bin/bash
#
# run_p2c.sh - container entrypoint for the OVMX/NetBSD P2c harness (rd vms-4b4,
# parent vms-dd8, epic vms-8e8). Sibling of run_p2b.sh: where P2b builds + loads
# a real in-kernel /dev/vms and proves one PING ioctl round-trips, P2c proves
# ONE real VMS EXECUTIVE FACILITY -- the COMMON EVENT FLAG CLUSTERS -- holds
# SYSTEM-WIDE SHARED state in the kernel, so a flag set by one process is seen by
# a DIFFERENT process (the INV-6-decisive proof, CLAUDE.md Rule 9), plus the
# module-absent honest-failure negative control.
#
#   docker run --rm -v <cache>:/cache [--device /dev/kvm] \
#     --entrypoint /netbsd/run_p2c.sh ovmx-netbsd-ktest
#
# HARD TIMEOUT. Every qemu invocation lives under the `timeout` below. A COLD
# cache pays a full download + sysinst install that also fetches comp + syssrc
# (kernel sources), so the default cap is larger; a WARM cache just boots, builds
# one small module + one small tool, and runs the cross-process proof.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

# shellcheck disable=SC1091
. "$HERE/netbsd_version.env"
export NETBSD_VERSION NETBSD_ARCH NETBSD_URL NETBSD_BOOT_ISO NETBSD_BOOT_ISO_SHA512

HARNESS_TIMEOUT="${HARNESS_TIMEOUT:-5400}"
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"

echo "======================================================================"
echo "  OVMX/NetBSD amd64 P2c harness (vms-4b4, epic vms-8e8)"
echo "  build + modload a real in-kernel /dev/vms with the COMMON EVENT FLAG"
echo "  facility; prove cross-process shared kernel state (A sets, B reads)"
echo "  through kif_transport_netbsd.c; plus the INV-6 negative control"
echo "  wall-clock cap: ${HARNESS_TIMEOUT}s"
echo "======================================================================"
qemu-system-x86_64 --version | head -1 || true
echo ""

set +e
timeout --kill-after="${TIMEOUT_GRACE}" "${HARNESS_TIMEOUT}" \
    python3 "$HERE/drive_netbsd_p2c.py"
RC=$?
set -e

echo ""
if [ "$RC" -eq 0 ]; then
    echo "======================================================================"
    echo "  NetBSD/amd64 P2c HARNESS PASSED"
    echo "======================================================================"
    exit 0
fi

if [ "$RC" -eq 124 ] || [ "$RC" -eq 137 ]; then
    echo "======================================================================"
    echo "  NetBSD/amd64 P2c HARNESS FAILED -- wall-clock timeout (${HARNESS_TIMEOUT}s)"
    echo "======================================================================"
else
    echo "======================================================================"
    echo "  NetBSD/amd64 P2c HARNESS FAILED (drive_netbsd_p2c.py exit ${RC})"
    echo "======================================================================"
fi
exit 1
