#!/bin/bash
# run_tests.sh - Host-side: boot QEMU, capture serial output, parse results
#
# This runs inside the Docker container. It launches QEMU with the kernel
# and initramfs built during docker build, captures all serial output, and
# exits with 0 if all tests pass, 1 otherwise.

set -euo pipefail

# The verdict lives in a sourceable helper (rd vms-b1f) so that run_tests.sh
# and the controls that exercise it cannot drift, and so that the decision is
# testable without booting QEMU. It takes a FILE, deliberately: the defect it
# replaces was `echo "$OUTPUT" | grep -q ...` under pipefail, where grep exits
# on match without draining, echo takes SIGPIPE, and the pipeline reports 141 —
# inverting the verdict on any run with more than a pipe buffer of output after
# the matching line. Do not reintroduce a pipeline here.
. "$(dirname "$0")/lib/harness_verdict.sh"

TIMEOUT=120
KERNEL=/boot/vmlinuz
INITRD=/initramfs.cpio.gz
ARCH=$(uname -m)

# Select QEMU binary and machine config based on architecture
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

echo "=== OVMX QEMU Test Runner ==="
echo "Architecture: $ARCH"
echo "QEMU: $QEMU"
echo "Kernel: $KERNEL ($(ls -lh $KERNEL | awk '{print $5}'))"
echo "Initrd: $INITRD ($(ls -lh $INITRD | awk '{print $5}'))"
echo ""

# Run QEMU with serial on stdio, capture all output
OUTPUT=$(timeout "$TIMEOUT" $QEMU \
    $MACHINE \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -nographic \
    -append "$CONSOLE panic=-1 loglevel=4" \
    -m 256M \
    -no-reboot \
    -smp 1 \
    -nic none \
    -nodefaults \
    -serial stdio \
    2>&1) || true

# Print full output
echo "$OUTPUT"
echo ""

# Parse final results line.
#
# THE ZERO MUST BE A WHOLE NUMBER. This was `FINAL RESULTS:.*0 suites failed`,
# which is satisfied by the trailing 0 of ANY count ending in zero -- so
# "10 suites failed" reported ALL KERNEL MODULE TESTS PASSED and exited 0.
# Latent since the harness was written; it first fires on the vms-a35 merge
# product, where vms-8019's test_kmod_procnam, vms-0ff's test_kmod_pin and
# vms-d0b's test_kmod_devtab together push the executive-absent failure count
# into double digits. Two ways it lies, both fatal to the barrier:
#   - the positive kernel-executive job goes green with 10 (or 20) real
#     failures, which is the whole facility silently untested;
#   - the negative-control job asserts a NONZERO exit and gets 0, so it fails
#     with "expected the harness to fail, but it exited 0".
# [^0-9] before the 0 requires the zero to stand alone.
OUTPUT_FILE=$(mktemp) || { echo "run_tests.sh: mktemp failed" >&2; exit 2; }
trap 'rm -f "$OUTPUT_FILE"' EXIT
printf '%s\n' "$OUTPUT" > "$OUTPUT_FILE"

if harness_verdict_zero_failures "$OUTPUT_FILE"; then
    echo "=========================================="
    echo "  ALL KERNEL MODULE TESTS PASSED"
    echo "=========================================="
    exit 0
else
    echo "=========================================="
    echo "  KERNEL MODULE TESTS FAILED"
    echo "=========================================="
    # Show individual test results for easy diagnosis
    echo ""
    echo "Individual test results:"
    grep -E "(PASS|FAIL):" "$OUTPUT_FILE" || true
    exit 1
fi
