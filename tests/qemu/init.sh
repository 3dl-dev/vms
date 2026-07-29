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

# Run each test program
for test in /tests/test_kmod_*; do
    [ -x "$test" ] || continue
    echo ""
    echo "--- $(basename $test) ---"
    "$test"
    rc=$?
    if [ $rc -eq 0 ]; then
        TOTAL_PASS=$((TOTAL_PASS+1))
    else
        TOTAL_FAIL=$((TOTAL_FAIL+1))
    fi
done

# Measurement program for the vms-ln0 logical-name placement ruling.
# Not a correctness test, but it does assert that /dev/vms answers before it
# times anything -- so it counts as a suite: a silent skip would be worse.
if [ -x /tests/bench_lnm_cost ]; then
    echo ""
    echo "--- bench_lnm_cost (vms-ln0 measurement) ---"
    /tests/bench_lnm_cost
    if [ $? -eq 0 ]; then
        TOTAL_PASS=$((TOTAL_PASS+1))
    else
        TOTAL_FAIL=$((TOTAL_FAIL+1))
    fi
fi

if [ -x /tests/bench_lnm_peropen ]; then
    echo ""
    echo "--- bench_lnm_peropen (vms-ln0 measurement) ---"
    /tests/bench_lnm_peropen
    if [ $? -eq 0 ]; then
        TOTAL_PASS=$((TOTAL_PASS+1))
    else
        TOTAL_FAIL=$((TOTAL_FAIL+1))
    fi
fi

echo ""
echo "=== FINAL RESULTS: $TOTAL_PASS suites passed, $TOTAL_FAIL suites failed ==="
echo ""

# Show kernel log for debugging
echo "--- dmesg (vms/vmsfs) ---"
dmesg | grep -iE 'vms|vmsfs' || true
echo "--- end dmesg ---"

# Exit QEMU (with -no-reboot, reboot causes QEMU to exit)
reboot -f
