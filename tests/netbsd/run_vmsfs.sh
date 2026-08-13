#!/bin/bash
#
# run_vmsfs.sh - container entrypoint for the OVMX/NetBSD ODS-2 mount+read
# harness (rd vms-308, epic vms-8e8; P4-VFS V4). The ODS-2 sibling of run_p2c.sh:
# where P2c builds + loads the executive `vms' pseudo-device and proves shared
# event-flag state, THIS boots NetBSD/amd64, masters an OVMX ODS-2 volume,
# builds + loads the ODS-2 vnode module, MOUNTS the volume read-only and READS
# HELLO.TXT out of it -- proving the shared src/kernel-core/vmsfs core mounts +
# reads on NetBSD through the NetBSD vnode backend.
#
#   docker run --rm -v <cache>:/cache [--device /dev/kvm] \
#     --entrypoint /netbsd/run_vmsfs.sh ovmx-netbsd-ktest
#
# HARD TIMEOUT. Every qemu invocation lives under the `timeout` below. This is a
# NIGHTLY proof (schedule/dispatch, not per-PR): the per-PR NetBSD vmsfs gate is
# the fast QEMU-free cross-compile (tests/netbsd/crosscompile-vmsfs.sh).

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

# shellcheck disable=SC1091
. "$HERE/netbsd_version.env"
export NETBSD_VERSION NETBSD_ARCH NETBSD_URL NETBSD_BOOT_ISO NETBSD_BOOT_ISO_SHA512

HARNESS_TIMEOUT="${HARNESS_TIMEOUT:-5400}"
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"

echo "======================================================================"
echo "  OVMX/NetBSD amd64 ODS-2 mount+read harness (vms-308, epic vms-8e8)"
echo "  master an OVMX ODS-2 volume; build + modload the ODS-2 vnode module;"
echo "  mount it read-only and read HELLO.TXT through the shared vmsfs core;"
echo "  plus the module-absent mount-fails negative control"
echo "  wall-clock cap: ${HARNESS_TIMEOUT}s"
echo "======================================================================"
qemu-system-x86_64 --version | head -1 || true
echo ""

set +e
timeout --kill-after="${TIMEOUT_GRACE}" "${HARNESS_TIMEOUT}" \
    python3 "$HERE/drive_netbsd_vmsfs.py"
RC=$?
set -e

echo ""
if [ "$RC" -eq 0 ]; then
    echo "======================================================================"
    echo "  NetBSD/amd64 ODS-2 mount HARNESS PASSED"
    echo "======================================================================"
    exit 0
fi

if [ "$RC" -eq 124 ] || [ "$RC" -eq 137 ]; then
    echo "======================================================================"
    echo "  NetBSD/amd64 ODS-2 mount HARNESS FAILED -- wall-clock timeout (${HARNESS_TIMEOUT}s)"
    echo "======================================================================"
else
    echo "======================================================================"
    echo "  NetBSD/amd64 ODS-2 mount HARNESS FAILED (drive_netbsd_vmsfs.py exit ${RC})"
    echo "======================================================================"
fi
exit 1
