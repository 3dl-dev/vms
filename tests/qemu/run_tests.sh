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

# Whole-VM wall budget (rd vms-055). This is ONE QEMU boot that runs every
# executive suite (~76 as of this writing) sequentially -- there is no
# per-suite timeout, only this single wall. It was 120s, and the suite set
# outgrew it: measured under CI contention the guest was SIGTERM'd around suite
# ~58/76 (e.g. test_syssvc_procnam) with ZERO failing assertions -- a pure
# capacity red, not a suite defect, and load-dependent (it passed on 2 of 3
# recent main runs). The work-floor is ~120s * 76/58 ~= 157s.
#
# 300 -> 600 (rd vms-4003, extends vms-055). 300s fixed the common case (a
# normal GitHub runner does the full 76-suite run in ~90s), but GitHub's TCG
# runner quality varies wildly and a pathologically slow (~5x) runner reached
# only 60/76 in 300s -- extrapolating, the full set needs ~380s there. 600s
# covers that observed worst case (60 suites in 300s -> 76 ~= 380s) with ~1.5x
# margin, while the ci.yml kernel-executive job's own timeout-minutes: 60
# remains the OUTER bound so a genuine hang (not mere slowness) still surfaces.
# A wall overrun is detected (timeout rc=124) and reported legibly below, but
# STILL fails the gate -- see the verdict path.
#
# 600 -> 1500 (vms-904, extends vms-4003/vms-055). The suite set has GROWN
# past what 600s covers, not because any one suite regressed but because the
# self-host spine landed HEAVY build-driving suites that each cost tens of
# seconds under TCG -- test_syssvc_mmk_build alone drives MMK.EXE compiling a
# real TU with TCC.EXE inside the guest, and test_syssvc_imgact_{realimg,
# nonres,tls,extern} each activate a real image. MEASURED at this change: a
# pure-TCG run (no KVM, as CI boots) completed only 59 suites before the 600s
# wall fired -- on this host AND on CI (both SIGTERM'd right after
# test_syssvc_procctl), and the KE gate was already failing on main for this
# reason (e702905d, 7fba9da5). Extrapolating 59 suites/600s to the full ~80
# gives ~815s; 1500s covers that with ~1.8x margin for TCG runner variance,
# while ci.yml's timeout-minutes: 60 stays the OUTER bound so a genuine hang
# still surfaces. The DURABLE fix is sharding the ~80 suites across N parallel
# VM jobs (rd vms-ea7 / vms-4003), which this interim wall raise does not
# replace -- it only stops a capacity red from masquerading as a suite defect.
TIMEOUT=1500
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

# Two blank virtio disks (vms-3e8). The executive enumerates the node's virtio
# block devices into DK units (DKA0: from vda, DKA100: from vdb) at module init
# -- test_kmod_disk asserts that against a real vms.ko, so the guest must
# actually HAVE two virtio disks. Raw sparse files, never mounted or formatted:
# the test reads only the unit->backing-device mapping, so their content does
# not matter. Cleaned up with the transcript below (ONE trap, all temp files --
# see the trap note further down).
OVMX_DISK0=$(mktemp) || { echo "run_tests.sh: mktemp failed" >&2; exit 2; }
OVMX_DISK1=$(mktemp) || { echo "run_tests.sh: mktemp failed" >&2; exit 2; }
truncate -s 16M "$OVMX_DISK0" "$OVMX_DISK1"
trap 'rm -f "$ASSERT_TRANSCRIPT" "$OVMX_DISK0" "$OVMX_DISK1"' EXIT

# One virtio-net NIC (vms-9d2). Exactly as the two virtio disks above give the
# executive real block devices to enumerate into DK units, this gives it a real
# Ethernet net device to enumerate into the VMS device ETH0: -- test_kmod_devtab
# / test_syssvc_getdvi / test_syssvc_showdev assert ETH0: against a real vms.ko,
# so the guest must actually HAVE a NIC. Added to the QEMU line below as
# -netdev user (SLIRP, zero host config, unprivileged/CI-safe) + a
# virtio-net-pci device with romfile= to suppress the PXE ROM (no boot pause),
# the same pair distro/boot/run-qemu.sh ships (vms-7bd). The executive sources
# the device through the GENERIC netdev abstraction, so virtio-net here stands in
# for whatever NIC a real deployment has -- the code path is driver-agnostic.

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

# Run QEMU with serial on stdio, capture all output.
# Capture timeout(1)'s exit code instead of discarding it: rc=124 means the
# whole-VM wall fired (SIGTERM on budget exhaustion), which we render legibly
# in the verdict path below (rd vms-055) rather than letting it masquerade as a
# suite assertion failure. `|| QEMU_RC=$?` keeps `set -e` from aborting here
# exactly as the old `|| true` did; there is no pipe, so pipefail is not in play.
QEMU_RC=0
OUTPUT=$(timeout "$TIMEOUT" $QEMU \
    $MACHINE \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -nographic \
    -append "$CONSOLE panic=-1 loglevel=4" \
    -m 256M \
    -no-reboot \
    -smp 1 \
    -netdev user,id=net0 \
    -device virtio-net-pci,netdev=net0,romfile= \
    -nodefaults \
    -serial stdio \
    "${SECOND_SERIAL[@]}" \
    -drive if=none,id=ovmxdisk0,file="$OVMX_DISK0",format=raw \
    -device virtio-blk-pci,drive=ovmxdisk0 \
    -drive if=none,id=ovmxdisk1,file="$OVMX_DISK1",format=raw \
    -device virtio-blk-pci,drive=ovmxdisk1 \
    2>&1) || QEMU_RC=$?

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
trap 'rm -f "$OUTPUT_FILE" "$ASSERT_TRANSCRIPT" "$OVMX_DISK0" "$OVMX_DISK1"' EXIT
printf '%s\n' "$OUTPUT" > "$OUTPUT_FILE"

if harness_verdict_zero_failures "$OUTPUT_FILE"; then
    echo "=========================================="
    echo "  ALL KERNEL MODULE TESTS PASSED"
    echo "=========================================="
    exit 0
else
    # Make a wall-timeout LEGIBLE rather than silent (rd vms-055). timeout(1)
    # returns 124 when it SIGTERMs the guest on budget exhaustion. Before this,
    # a budget overrun exited nonzero indistinguishably from a real suite
    # failure -- the transcript was simply truncated before FINAL RESULTS, so
    # the verdict helper reported failure with no hint that the cause was the
    # clock, not an assertion. We name it here. Crucially this does NOT swallow
    # the failure: a genuine overrun must still redden the gate, just legibly,
    # so we fall through to the same exit 1 below. Every other nonzero exit
    # (QEMU_RC != 124, or a real FAIL line) stays a genuine suite failure.
    if [ "$QEMU_RC" -eq 124 ]; then
        echo "=========================================="
        echo "  KE VM WALL BUDGET (${TIMEOUT}s) EXCEEDED"
        echo "=========================================="
        echo "run_tests.sh: the whole-VM wall timeout fired (timeout rc=124) before"
        echo "the suite run reached its own FINAL RESULTS accounting. This is a HARNESS"
        echo "CAPACITY issue (rd vms-055), NOT a suite assertion failure -- assertions"
        echo "that DID run may all have passed. The gate still fails (a real overrun"
        echo "must not be swallowed). If the suite set has genuinely outgrown ${TIMEOUT}s,"
        echo "raise TIMEOUT in tests/qemu/run_tests.sh; do not drop or skip a suite."
        echo "If ${TIMEOUT}s ever proves insufficient, the SCALABLE fix (not implemented"
        echo "here, rd vms-4003) is to SHARD the ~76 suites across N parallel VM jobs --"
        echo "each shard boots its own QEMU over a suite subset, so its wall trivially"
        echo "covers that subset regardless of runner speed; raising a single whole-VM"
        echo "wall only buys linear headroom against an unbounded-slow TCG runner."
        echo ""
    fi
    echo "=========================================="
    echo "  KERNEL MODULE TESTS FAILED"
    echo "=========================================="
    # Show individual test results for easy diagnosis
    echo ""
    echo "Individual test results:"
    grep -E "(PASS|FAIL):" "$OUTPUT_FILE" || true
    exit 1
fi
