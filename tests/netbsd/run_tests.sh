#!/bin/bash
#
# run_tests.sh - container entrypoint for the OVMX/NetBSD amd64 harness.
# (rd vms-7f7, Phase 2a of epic vms-8e8.) The NetBSD-substrate sibling of
# tests/qemu/run_tests.sh.
#
# Boots NetBSD/amd64 under qemu-system-x86_64 (via anita), drives the serial
# console, asserts `uname -srm` == "NetBSD <version> amd64", and exits 0 on
# success / nonzero on any failure or timeout.
#
#   docker run --rm -v <cache>:/cache [--device /dev/kvm] ovmx-netbsd-ktest
#
# HARD TIMEOUT. Every qemu invocation lives underneath the `timeout` below: a
# prior QEMU proof in this repo once ran unbounded for 1h43m, so this harness
# refuses to. HARNESS_TIMEOUT is the wall-clock cap for the whole run (install +
# boot + assert); drive_netbsd.py additionally imposes a tighter internal boot
# deadline (NETBSD_BOOT_DEADLINE) so a hung boot fails fast rather than eating
# the whole budget. `timeout` sends SIGTERM (which drive_netbsd.py catches to
# reap qemu), then SIGKILL after the grace period.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

# shellcheck disable=SC1091
. "$HERE/netbsd_version.env"
export NETBSD_VERSION NETBSD_ARCH NETBSD_URL NETBSD_BOOT_ISO NETBSD_BOOT_ISO_SHA512

# A cold cache under TCG (no KVM, i.e. GitHub-hosted runners) does a full
# download + sysinst install of NetBSD, which is slow; the default cap is sized
# for that. A warm cache (image already installed) boots in a couple of minutes.
# Override with -e HARNESS_TIMEOUT=<seconds>.
HARNESS_TIMEOUT="${HARNESS_TIMEOUT:-3600}"
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"

echo "======================================================================"
echo "  OVMX/NetBSD amd64 harness (vms-7f7, epic vms-8e8)"
echo "  NetBSD ${NETBSD_VERSION}/${NETBSD_ARCH} under qemu-system-x86_64 via anita"
echo "  assertion: uname -srm == 'NetBSD ${EXPECT_VERSION:-$NETBSD_VERSION} ${EXPECT_ARCH:-$NETBSD_ARCH}'"
echo "  wall-clock cap: ${HARNESS_TIMEOUT}s"
echo "======================================================================"
qemu-system-x86_64 --version | head -1 || true
echo ""

set +e
timeout --kill-after="${TIMEOUT_GRACE}" "${HARNESS_TIMEOUT}" \
    python3 "$HERE/drive_netbsd.py"
RC=$?
set -e

echo ""
if [ "$RC" -eq 0 ]; then
    echo "======================================================================"
    echo "  NetBSD/amd64 HARNESS PASSED"
    echo "======================================================================"
    exit 0
fi

if [ "$RC" -eq 124 ] || [ "$RC" -eq 137 ]; then
    echo "======================================================================"
    echo "  NetBSD/amd64 HARNESS FAILED -- wall-clock timeout (${HARNESS_TIMEOUT}s)"
    echo "======================================================================"
else
    echo "======================================================================"
    echo "  NetBSD/amd64 HARNESS FAILED (drive_netbsd.py exit ${RC})"
    echo "======================================================================"
fi
exit 1
