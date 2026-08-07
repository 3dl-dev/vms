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

# ASSERTION TRANSCRIPT (vms-b5b round 2). ttyS0 (below) carries the boot
# banner, kernel printk and init.sh's own aggregate lines -- exactly what it
# always has. A SECOND serial port, ttyS1, is wired to a plain file so that
# init.sh's per-suite loop (which writes there via fd 4 -- see init.sh) has a
# wire kernel printk never touches, closing the printk-splice class the
# round-1 audit found (tee-to-console put suite text and printk on the SAME
# wire; see init.sh's header comment for the measured line). x86_64 only:
# CI's kernel-executive/negctl jobs run exclusively on x86_64 GitHub runners,
# so that is the only path this has been verified against a real /dev/vms;
# the aarch64 branch below is left on the single-console (round-1) shape,
# disclosed, not silently assumed fixed -- see the comment there.
ASSERT_TRANSCRIPT=$(mktemp) || { echo "run_tests.sh: mktemp failed" >&2; exit 2; }
trap 'rm -f "$ASSERT_TRANSCRIPT"' EXIT

# Select QEMU binary and machine config based on architecture
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
    # NOT verified: mach-virt provides exactly one PL011 UART by default: a
    # second port needs an explicit -device pl011 at a free MMIO address,
    # which this repo has no aarch64 QEMU host to test against (tracked as
    # rd vms-c83). init.sh's fallback (fd 4 -> fd 1 when /dev/ttyS1 is
    # absent) keeps this path running exactly as round 1 left it -- flaky
    # under printk, LOUDLY disclosed on the console, not silently "fixed".
    SECOND_SERIAL=()
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
    # The PC machine wires an Nth -serial to COM(N+1) automatically -- no
    # -device needed, the same convenience the existing ttyS0 stdio backend
    # already relies on. This is the path CI actually boots.
    SECOND_SERIAL=(-serial "file:$ASSERT_TRANSCRIPT")
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
    "${SECOND_SERIAL[@]}" \
    2>&1) || true

# Splice the assertion transcript (ttyS1, if this arch has one) back into
# $OUTPUT right after init.sh's own banner announcing it took that channel.
# init.sh's loop prints NOTHING ELSE to ttyS0 for the whole loop's duration
# (every per-suite line went to fd 4 instead, see init.sh) -- so there is no
# "gap" content on ttyS0 to remove, only a single insertion point to fill.
# When the banner is absent (fallback: /dev/ttyS1 was never wired, e.g. the
# aarch64 branch above), $OUTPUT already carries everything inline exactly
# as round 1 left it, and this is a no-op.
ASSERT_BANNER='--- assertion channel: /dev/ttyS1 (separate from console) ---'
if printf '%s\n' "$OUTPUT" | grep -qF -- "$ASSERT_BANNER"; then
    OUTPUT=$(printf '%s\n' "$OUTPUT" | awk -v bannerfile="$ASSERT_TRANSCRIPT" -v banner="$ASSERT_BANNER" '
        { print }
        index($0, banner) == 1 {
            while ((getline line < bannerfile) > 0) print line
            close(bannerfile)
        }
    ')
fi

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
# ONE trap, both temp files: a second `trap ... EXIT` REPLACES the first
# rather than stacking with it, so registering ASSERT_TRANSCRIPT's cleanup
# separately above would have silently dropped it the moment this line ran.
trap 'rm -f "$OUTPUT_FILE" "$ASSERT_TRANSCRIPT"' EXIT
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
