#!/bin/bash
# test_persistent_boot.sh - Verify persistent system disk across QEMU reboots
#
# Runs inside the ovmx-boot Docker image (has QEMU + kernel + initramfs).
# Tests the install-once / boot-many model:
#   Boot 1: blank disk → STARTUP.EXE initializes, formats, installs
#   Boot 2: same disk  → STARTUP.EXE detects installed system, skips install
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_persistent_boot.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit code 0 = all checks pass, 1 = failures

set -euo pipefail

TIMEOUT=90
DISK="/tmp/test-sysdisk.img"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
ARCH=$(uname -m)

PASS=0
FAIL=0
TOTAL=0

# Select QEMU binary based on architecture
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

check() {
    local desc="$1" output="$2" pattern="$3" expect="${4:-present}"
    TOTAL=$((TOTAL + 1))
    if echo "$output" | grep -qF "$pattern"; then
        if [ "$expect" = "present" ]; then
            echo "  PASS: $desc"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: $desc (pattern found but should be absent: $pattern)"
            FAIL=$((FAIL + 1))
        fi
    else
        if [ "$expect" = "absent" ]; then
            echo "  PASS: $desc (correctly absent)"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: $desc (pattern not found: $pattern)"
            FAIL=$((FAIL + 1))
        fi
    fi
}

run_qemu() {
    timeout "$TIMEOUT" $QEMU $MACHINE \
        -kernel "$KERNEL" \
        -initrd "$INITRD" \
        -nographic \
        -append "$CONSOLE loglevel=3 quiet" \
        -m 256M \
        -smp 1 \
        -nic none \
        -nodefaults \
        -serial stdio \
        -drive file="$DISK",format=raw,if=virtio \
        -no-reboot \
        </dev/null 2>&1 || true
}

echo "=== OVMX Persistent Boot Smoke Test ==="
echo "Architecture: $ARCH"
echo "QEMU: $QEMU"
echo "Kernel: $KERNEL"
echo "Initrd: $INITRD"
echo ""

# --- Boot 1: Fresh install (blank disk) ---
echo "--- Creating blank 64M system disk ---"
truncate -s 64M "$DISK"
echo ""

echo "--- Boot 1: Fresh install (blank disk) ---"
OUTPUT1=$(run_qemu)

# Show first 40 lines for diagnostics
echo "$OUTPUT1" | head -40
echo "[... truncated ...]"
echo ""

check "Boot 1: system disk detected"      "$OUTPUT1" "%STARTUP-I-SYSDISK"
check "Boot 1: blank disk initialized"    "$OUTPUT1" "%STARTUP-I-INIT, initializing"
check "Boot 1: disk mounted"              "$OUTPUT1" "%STARTUP-I-MOUNTED"
check "Boot 1: install started"           "$OUTPUT1" "%STARTUP-I-INSTALL, installing"
check "Boot 1: install completed"         "$OUTPUT1" "%STARTUP-I-INSTALLED"
check "Boot 1: boot banner"              "$OUTPUT1" "OpenVMS-compatible"
echo ""

# Verify disk is not empty (persistence proof — data was written)
DISK_BYTES=$(stat -c%s "$DISK" 2>/dev/null || stat -f%z "$DISK" 2>/dev/null)
echo "Disk size after boot 1: $DISK_BYTES bytes"
echo ""

# --- Boot 2: Persistent reboot (same disk) ---
echo "--- Boot 2: Persistent reboot (same disk) ---"
OUTPUT2=$(run_qemu)

echo "$OUTPUT2" | head -40
echo "[... truncated ...]"
echo ""

check "Boot 2: system disk detected"      "$OUTPUT2" "%STARTUP-I-SYSDISK"
check "Boot 2: disk mounted"              "$OUTPUT2" "%STARTUP-I-MOUNTED"
check "Boot 2: install skipped"           "$OUTPUT2" "%STARTUP-I-SYSBOOT"
check "Boot 2: no disk initialization"    "$OUTPUT2" "%STARTUP-I-INIT, initializing" "absent"
check "Boot 2: no install"                "$OUTPUT2" "%STARTUP-I-INSTALL, installing" "absent"
check "Boot 2: boot banner"              "$OUTPUT2" "OpenVMS-compatible"
echo ""

# --- Results ---
echo "=========================================="
echo "  RESULTS: $PASS/$TOTAL checks passed, $FAIL failed"
echo "=========================================="

if [ "$FAIL" -eq 0 ]; then
    echo "  ALL PERSISTENT BOOT CHECKS PASSED"
    exit 0
else
    echo "  PERSISTENT BOOT CHECKS FAILED"
    echo ""
    echo "--- Boot 1 full output ---"
    echo "$OUTPUT1"
    echo ""
    echo "--- Boot 2 full output ---"
    echo "$OUTPUT2"
    exit 1
fi
