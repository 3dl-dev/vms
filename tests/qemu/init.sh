#!/bin/busybox sh
# init.sh - PID 1 inside QEMU initramfs
# Loads vms.ko, runs kernel module test programs, exits.

# Install busybox applets (mount, insmod, reboot, etc.)
/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

echo ""
echo "=== OVMX Kernel Module Test Suite ==="
echo "Kernel: $(uname -r) ($(uname -m))"
echo ""

TOTAL_PASS=0
TOTAL_FAIL=0

# Set up loop device for block-device vmsfs test
# loop may be built-in (CONFIG_BLK_DEV_LOOP=y) or a module
if [ -f /lib/modules/loop.ko ]; then
    echo "--- Loading loop.ko ---"
    insmod /lib/modules/loop.ko
fi
if [ -f /test_data/vmsfs_test.img ]; then
    echo "--- Setting up loop device for blkdev test ---"
    # Create /dev/loop0 if devtmpfs didn't auto-create it
    [ -b /dev/loop0 ] || mknod /dev/loop0 b 7 0
    losetup /dev/loop0 /test_data/vmsfs_test.img
    if [ -b /dev/loop0 ]; then
        echo "  OK: loop0 attached to vmsfs_test.img"
    else
        echo "  WARN: losetup failed or /dev/loop0 not present"
    fi
fi

# Load vms.ko
echo "--- Loading vms.ko ---"
insmod /lib/modules/vms.ko
if [ -c /dev/vms ]; then
    echo "  PASS: vms.ko loaded, /dev/vms present"
    TOTAL_PASS=$((TOTAL_PASS+1))
else
    echo "  FAIL: vms.ko load or /dev/vms creation failed"
    TOTAL_FAIL=$((TOTAL_FAIL+1))
    # Show dmesg for diagnostics
    dmesg | tail -20
fi

# Load vmsfs.ko
echo "--- Loading vmsfs.ko ---"
insmod /lib/modules/vmsfs.ko
if grep -q vmsfs /proc/filesystems; then
    echo "  PASS: vmsfs.ko loaded, filesystem registered"
    TOTAL_PASS=$((TOTAL_PASS+1))
else
    echo "  FAIL: vmsfs.ko load or filesystem registration failed"
    TOTAL_FAIL=$((TOTAL_FAIL+1))
    dmesg | tail -20
fi

# Create directories needed by vmsfs tests
mkdir -p /tmp/vmsfs_backing /mnt/vmsfs

# Run each test program. test_kmod_* drive /dev/vms with raw ioctls
# (kernel lock manager, ASTs, event flags, access modes, vmsfs). test_syssvc_*
# drive the same /dev/vms through the PUBLIC sys$ API in src/libvms instead
# (vms-1d9) -- exercising the userspace system-service layer the ioctl tests
# cannot see at all.
#
# PER-SUITE VERDICT LINE (vms-1d9). After each suite we print
#
#     === SUITE <name> rc=<exit code> ===
#
# and .github/workflows/ci.yml asserts on THAT, per suite, instead of on the
# aggregate "FINAL RESULTS" tally below. Two real defects made this necessary,
# both proven against running artifacts, not argued:
#
#  1. The aggregate tally cannot distinguish an honest skip (rc 77) from a
#     failed assertion (rc 1) -- the two branches below both increment
#     TOTAL_FAIL. Injecting a FABRICATED SUCCESS into
#     src/libvms/syssvc/sys_lock.c (do_enq and sys$deq returning SS$_NORMAL
#     with an invented lock ID when the executive was never reached) makes
#     every one of test_syssvc_lock's device-absent assertions FAIL and its
#     exit code change 77 -> 1 -- while the negative-control run's FINAL
#     RESULTS line stays BYTE-IDENTICAL to the clean tree
#     ("3 suites passed, 11 suites failed"). Measured, both ways, on this
#     harness. A per-process fake reporting success is invisible to any
#     gate that pins the tally; the per-suite rc catches it.
#
#  2. Any assertion on the aggregate count is either a pin that turns CI red
#     when a legitimate new suite is ADDED, or a floor that stops protecting
#     every suite added after it was written. Per-suite lines let CI derive
#     the expected set from the checkout (`ls tests/qemu/test_*.c`), which is
#     addition-tolerant AND drop-detecting with nothing maintained by hand.
#
# The TOTAL_PASS/TOTAL_FAIL tally is kept for human readers and for
# run_tests.sh's exit code; it is no longer the thing CI pins.
for test in /tests/test_kmod_* /tests/test_syssvc_*; do
    [ -x "$test" ] || continue
    name=$(basename "$test")
    echo ""
    echo "--- $name ---"
    "$test"
    rc=$?
    if [ $rc -eq 0 ]; then
        TOTAL_PASS=$((TOTAL_PASS+1))
    elif [ $rc -eq 77 ]; then
        # Honest skip (e.g. /dev/vms absent) -- should never happen in this
        # job, since vms.ko was just insmod'd above. Count as a FAIL: if it
        # ever fires here, the executive is not actually present, which is
        # exactly what this job exists to catch.
        echo "  SKIP reported inside the kernel-executive job -- treating as FAIL"
        TOTAL_FAIL=$((TOTAL_FAIL+1))
    else
        TOTAL_FAIL=$((TOTAL_FAIL+1))
    fi
    echo "=== SUITE $name rc=$rc ==="
done

echo ""
echo "=== FINAL RESULTS: $TOTAL_PASS suites passed, $TOTAL_FAIL suites failed ==="
echo ""

# Show kernel log for debugging
echo "--- dmesg (vms/vmsfs) ---"
dmesg | grep -iE 'vms|vmsfs' || true
echo "--- end dmesg ---"

# Exit QEMU (with -no-reboot, reboot causes QEMU to exit)
reboot -f
