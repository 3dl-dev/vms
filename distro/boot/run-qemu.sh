#!/bin/bash
# Boot OVMX in QEMU from extracted kernel + initramfs.
#
# Usage:
#   ./distro/boot/run-qemu.sh [kernel] [initrd]
#
# Environment variables:
#   MEMORY  - Guest RAM (default: 512M)

KERNEL="${1:-dist/boot/vmlinuz}"
INITRD="${2:-dist/boot/initramfs-ovmx.cpio.gz}"
MEMORY="${MEMORY:-512M}"
DISK="${DISK:-}"
ARCH=$(uname -m)

if [ ! -f "$KERNEL" ]; then
    echo "Error: kernel not found at $KERNEL" >&2
    echo "Build first: docker build -f distro/Dockerfile.bootable -o type=local,dest=dist ." >&2
    exit 1
fi

if [ ! -f "$INITRD" ]; then
    echo "Error: initramfs not found at $INITRD" >&2
    exit 1
fi

DRIVE_ARGS=""
if [ -n "$DISK" ]; then
    if [ ! -f "$DISK" ]; then
        echo "Creating blank system disk: $DISK (64MB)"
        truncate -s 64M "$DISK"
    fi
    DRIVE_ARGS="-drive file=$DISK,format=raw,if=virtio"
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
    $DRIVE_ARGS \
    -no-reboot
