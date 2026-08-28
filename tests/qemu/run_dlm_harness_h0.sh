#!/bin/bash
# run_dlm_harness_h0.sh - Host-side (runs INSIDE the Docker container built from
# tests/qemu/Dockerfile.dlm-harness): boot ONE QEMU node with a real executive,
# run SCSD.EXE --dlm-selftest, and verdict the H0 proof (rd vms-4b6).
#
# H0 PASS iff the boot transcript carries EXACTLY the line
#     H0-DLM-SCSD-EXEC: rc=2296 PASS
# i.e. SCSD's own scsd_dlm_dispatch_to_executive() reached a REAL /dev/vms and
# the executive's rung-1 DLM handler returned SS$_UNSUPPORTED (2296) -- NOT
# SS$_NOSUCHDEV (2680), the fail-honest status when no executive is present.
# The two-node harness (H1-H4) builds on this single-node foundation.

set -euo pipefail

TIMEOUT="${H0_WALL_TIMEOUT:-300}"
KERNEL=/boot/vmlinuz
INITRD=/initramfs.cpio.gz
ARCH=$(uname -m)

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    if [ -w /dev/kvm ]; then
        MACHINE="-accel kvm -cpu host"
    else
        MACHINE="-accel tcg"
    fi
    CONSOLE="console=ttyS0"
fi

echo "=== OVMX DLM Harness H0 Runner (vms-4b6) ==="
echo "Architecture: $ARCH"
echo "QEMU: $QEMU"
echo "Kernel: $KERNEL"
echo "Initrd: $INITRD"
echo ""

# One QEMU boot. No disks, no NIC: SCSD --dlm-selftest opens no socket -- it
# only open()s /dev/vms and issues the DLM ioctl. Everything the guest prints on
# ttyS0 (banner, kernel printk, init_dlm_h0.sh's verdict) is captured.
QEMU_RC=0
OUTPUT=$(timeout "$TIMEOUT" $QEMU \
    $MACHINE \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -nographic \
    -append "$CONSOLE panic=-1 loglevel=4" \
    -m 512M \
    -no-reboot \
    -smp 1 \
    -nodefaults \
    -serial stdio \
    2>&1) || QEMU_RC=$?

echo "$OUTPUT"
echo ""

if [ "$QEMU_RC" -eq 124 ]; then
    echo "=========================================="
    echo "  H0 VM WALL BUDGET (${TIMEOUT}s) EXCEEDED"
    echo "=========================================="
    echo "run_dlm_harness_h0.sh: the guest did not reach the H0 verdict before the"
    echo "wall timeout. This is a HARNESS CAPACITY issue, not necessarily a defect;"
    echo "raise H0_WALL_TIMEOUT. The gate still fails (an overrun must not be swallowed)."
    exit 1
fi

# THE ZERO/2296 MUST BE THE WHOLE TOKEN. Match the exact PASS verdict line so a
# 2680 FAIL (or any other status) cannot masquerade as a pass.
if printf '%s\n' "$OUTPUT" | grep -qE '^H0-DLM-SCSD-EXEC: rc=2296 PASS'; then
    echo "=========================================="
    echo "  DLM HARNESS H0 PASSED"
    echo "  SCSD.EXE reached the real executive: SS\$_UNSUPPORTED (2296), not SS\$_NOSUCHDEV (2680)"
    echo "=========================================="
    exit 0
else
    echo "=========================================="
    echo "  DLM HARNESS H0 FAILED"
    echo "=========================================="
    echo ""
    echo "H0 verdict / SCSD status lines:"
    printf '%s\n' "$OUTPUT" | grep -E 'H0-DLM-SCSD-EXEC|SCSD-I-DLMSELFTEST|/dev/vms' || true
    exit 1
fi
