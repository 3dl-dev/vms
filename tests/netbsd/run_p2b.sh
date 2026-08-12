#!/bin/bash
#
# run_p2b.sh - container entrypoint for the OVMX/NetBSD P2b harness (rd vms-bfe,
# parent vms-dd8, epic vms-8e8). Sibling of run_tests.sh (P2a): where P2a boots
# NetBSD and asserts `uname`, this one builds + loads a REAL in-kernel /dev/vms
# `vms' pseudo-device and proves one version/ping ioctl end to end through the
# transport seam (kif_transport_netbsd.c), plus the INV-6 module-absent negative
# control.
#
#   docker run --rm -v <cache>:/cache [--device /dev/kvm] \
#     --entrypoint /netbsd/run_p2b.sh ovmx-netbsd-ktest
#
# HARD TIMEOUT. Every qemu invocation lives under the `timeout` below. A COLD
# cache pays a full download + sysinst install that now also fetches comp +
# syssrc (kernel sources), so the default cap is larger than P2a's; a WARM cache
# just boots, builds one small module, and probes.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

# shellcheck disable=SC1091
. "$HERE/netbsd_version.env"
export NETBSD_VERSION NETBSD_ARCH NETBSD_URL NETBSD_BOOT_ISO NETBSD_BOOT_ISO_SHA512

HARNESS_TIMEOUT="${HARNESS_TIMEOUT:-5400}"
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"

echo "======================================================================"
echo "  OVMX/NetBSD amd64 P2b harness (vms-bfe, epic vms-8e8)"
echo "  build + modload a real in-kernel /dev/vms; assert one ping ioctl"
echo "  through kif_transport_netbsd.c; plus the INV-6 negative control"
echo "  wall-clock cap: ${HARNESS_TIMEOUT}s"
echo "======================================================================"
qemu-system-x86_64 --version | head -1 || true
echo ""

set +e
timeout --kill-after="${TIMEOUT_GRACE}" "${HARNESS_TIMEOUT}" \
    python3 "$HERE/drive_netbsd_p2b.py"
RC=$?
set -e

echo ""
if [ "$RC" -eq 0 ]; then
    echo "======================================================================"
    echo "  NetBSD/amd64 P2b HARNESS PASSED"
    echo "======================================================================"
    exit 0
fi

if [ "$RC" -eq 124 ] || [ "$RC" -eq 137 ]; then
    echo "======================================================================"
    echo "  NetBSD/amd64 P2b HARNESS FAILED -- wall-clock timeout (${HARNESS_TIMEOUT}s)"
    echo "======================================================================"
else
    echo "======================================================================"
    echo "  NetBSD/amd64 P2b HARNESS FAILED (drive_netbsd_p2b.py exit ${RC})"
    echo "======================================================================"
fi
exit 1
