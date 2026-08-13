#!/bin/bash
#
# run_p4a.sh - container entrypoint for the OVMX/NetBSD P4-A harness (rd
# vms-f8a, parent vms-dd8, epic vms-8e8). Sibling of run_p2c.sh: where P2c
# proves the event-flag facility is real, shared, cross-process kernel state,
# P4-A proves the same for the FIVE remaining executive facilities --
# process table, mailboxes, locks (DLM), access modes, and ASTs (via the
# mailbox write-attention integration) -- plus the module-absent INV-6
# honest-failure negative control for every new probe tool.
#
#   docker run --rm -v <cache>:/cache [--device /dev/kvm] \
#     --entrypoint /netbsd/run_p4a.sh ovmx-netbsd-ktest
#
# HARD TIMEOUT. Every qemu invocation lives under the `timeout` below. A COLD
# cache pays a full download + sysinst install that also fetches comp + syssrc
# (kernel sources); a WARM cache just boots, builds one module (now six
# facilities) + four small tools, and runs five cross-process proofs.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

# shellcheck disable=SC1091
. "$HERE/netbsd_version.env"
export NETBSD_VERSION NETBSD_ARCH NETBSD_URL NETBSD_BOOT_ISO NETBSD_BOOT_ISO_SHA512

HARNESS_TIMEOUT="${HARNESS_TIMEOUT:-5400}"
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"

echo "======================================================================"
echo "  OVMX/NetBSD amd64 P4-A harness (vms-f8a, epic vms-8e8)"
echo "  build + modload a real in-kernel /dev/vms with ALL SIX executive"
echo "  facilities; prove cross-process shared kernel state for proctab,"
echo "  mbx, lock, access and ast (eflag already proven by P2c); INV-6"
echo "  module-absent negative control for every new probe tool"
echo "  wall-clock cap: ${HARNESS_TIMEOUT}s"
echo "======================================================================"
qemu-system-x86_64 --version | head -1 || true
echo ""

set +e
timeout --kill-after="${TIMEOUT_GRACE}" "${HARNESS_TIMEOUT}" \
    python3 "$HERE/drive_netbsd_p4a.py"
RC=$?
set -e

echo ""
if [ "$RC" -eq 0 ]; then
    echo "======================================================================"
    echo "  NetBSD/amd64 P4-A HARNESS PASSED"
    echo "======================================================================"
    exit 0
fi

if [ "$RC" -eq 124 ] || [ "$RC" -eq 137 ]; then
    echo "======================================================================"
    echo "  NetBSD/amd64 P4-A HARNESS FAILED -- wall-clock timeout (${HARNESS_TIMEOUT}s)"
    echo "======================================================================"
else
    echo "======================================================================"
    echo "  NetBSD/amd64 P4-A HARNESS FAILED (drive_netbsd_p4a.py exit ${RC})"
    echo "======================================================================"
fi
exit 1
