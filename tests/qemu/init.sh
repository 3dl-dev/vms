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
for test in /tests/test_kmod_* /tests/test_syssvc_*; do
    [ -x "$test" ] || continue
    echo ""
    echo "--- $(basename $test) ---"
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
