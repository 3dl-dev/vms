#!/bin/bash
# Boot OVMX in QEMU from extracted kernel + initramfs.
#
# Usage:
#   ./distro/boot/run-qemu.sh [kernel] [initrd]
#
# Environment variables:
#   MEMORY  - Guest RAM (default: 512M)
#   DISK    - Path to system disk image (optional; passed as /dev/vda if set)
#
# Initramfs variants:
#   initramfs-ovmx.cpio.gz       — fat: all binaries (first boot / install)
#   initramfs-ovmx-slim.cpio.gz  — slim: bootstrap only (needs system disk)

KERNEL="${1:-dist/boot/vmlinuz}"
INITRD="${2:-dist/boot/initramfs-ovmx.cpio.gz}"
MEMORY="${MEMORY:-512M}"
ARCH=$(uname -m)

if [ ! -f "$KERNEL" ]; then
    echo "Error: kernel not found at $KERNEL" >&2
    echo "Build first: ./boot.sh" >&2
    exit 1
fi

if [ ! -f "$INITRD" ]; then
    echo "Error: initramfs not found at $INITRD" >&2
    exit 1
fi

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

# Build disk arguments if DISK is set
DISK_ARGS=""
if [ -n "$DISK" ]; then
    if [ ! -f "$DISK" ]; then
        echo "Error: disk image not found: $DISK" >&2
        exit 1
    fi
    DISK_ARGS="-drive file=$DISK,format=raw,if=virtio,cache=writeback"
fi

exec $QEMU $MACHINE \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -nographic \
    -append "$CONSOLE loglevel=3 quiet" \
    -m "$MEMORY" \
    -smp 2 \
    -nic none \
    -nodefaults \
    -serial mon:stdio \
    -no-reboot \
    $DISK_ARGS
