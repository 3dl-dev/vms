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

echo ""
echo "=== FINAL RESULTS: $TOTAL_PASS suites passed, $TOTAL_FAIL suites failed ==="
echo ""

# Show kernel log for debugging
echo "--- dmesg (vms/vmsfs) ---"
dmesg | grep -iE 'vms|vmsfs' || true
echo "--- end dmesg ---"

# Exit QEMU (with -no-reboot, reboot causes QEMU to exit)
reboot -f
