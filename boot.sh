#!/bin/bash
# Boot OVMX in QEMU with persistent system disk.
#
# Usage:
#   ./boot.sh                          # Build (if needed), boot with default disk
#   ./boot.sh --disk path/to/disk.img  # Boot with specified disk image
#   ./boot.sh --fresh                  # Force fresh disk (delete and recreate)
#   ./boot.sh --rebuild                # Force Docker rebuild + extract + boot
#   ./boot.sh --slim                   # Use slim initramfs (needs installed disk)
#
# First run:  Docker builds, extracts kernel+initramfs to dist/boot/,
#             creates blank dist/sysdisk.img, boots → STARTUP.EXE initializes.
# Later runs: Detects existing artifacts, skips build, boots → fast.
#
# Environment:
#   MEMORY  - Guest RAM (default: 512M)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DIST_DIR="$SCRIPT_DIR/dist"
BOOT_DIR="$DIST_DIR/boot"
DEFAULT_DISK="$DIST_DIR/sysdisk.img"
DISK_SIZE_MB=64

IMAGE="ovmx-boot"
MEMORY="${MEMORY:-512M}"

FORCE_REBUILD=0
FORCE_FRESH=0
USE_SLIM=0
DISK_PATH=""

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --rebuild|-r)
            FORCE_REBUILD=1
            shift
            ;;
        --fresh|-f)
            FORCE_FRESH=1
            shift
            ;;
        --slim|-s)
            USE_SLIM=1
            shift
            ;;
        --disk|-d)
            if [ -z "$2" ]; then
                echo "Error: --disk requires a path argument" >&2
                exit 1
            fi
            DISK_PATH="$2"
            shift 2
            ;;
        --disk=*)
            DISK_PATH="${1#--disk=}"
            shift
            ;;
        --help|-h)
            sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--rebuild] [--fresh] [--slim] [--disk path]" >&2
            exit 1
            ;;
    esac
done

# Resolve disk path
DISK="${DISK_PATH:-$DEFAULT_DISK}"

# --- Step 1: Build + extract kernel and initramfs ---

KERNEL="$BOOT_DIR/vmlinuz"
INITRD_FAT="$BOOT_DIR/initramfs-ovmx.cpio.gz"
INITRD_SLIM="$BOOT_DIR/initramfs-ovmx-slim.cpio.gz"

need_build=0
if [ "$FORCE_REBUILD" -eq 1 ]; then
    need_build=1
elif [ ! -f "$KERNEL" ] || [ ! -f "$INITRD_FAT" ]; then
    need_build=1
fi

if [ "$need_build" -eq 1 ]; then
    echo "=== Building OVMX bootable image ==="
    BUILD_ARGS=""
    [ "$FORCE_REBUILD" -eq 1 ] && BUILD_ARGS="--no-cache"
    docker build -f "$SCRIPT_DIR/distro/Dockerfile.bootable" \
        -t "$IMAGE" $BUILD_ARGS "$SCRIPT_DIR"
    echo "=== Build complete ==="

    echo "=== Extracting kernel and initramfs to $BOOT_DIR ==="
    mkdir -p "$BOOT_DIR"
    CID=$(docker create "$IMAGE")
    docker cp "$CID:/boot/vmlinuz" "$KERNEL"
    docker cp "$CID:/boot/initramfs-ovmx.cpio.gz" "$INITRD_FAT"
    docker cp "$CID:/boot/initramfs-ovmx-slim.cpio.gz" "$INITRD_SLIM" 2>/dev/null || true
    docker rm "$CID" >/dev/null
    echo "=== Extraction complete ==="
    echo ""
fi

# Select initramfs
if [ "$USE_SLIM" -eq 1 ] && [ -f "$INITRD_SLIM" ]; then
    INITRD="$INITRD_SLIM"
    echo "Using slim initramfs (bootstrap only)"
else
    INITRD="$INITRD_FAT"
fi

# --- Step 2: Prepare system disk ---

if [ "$FORCE_FRESH" -eq 1 ] && [ -z "$DISK_PATH" ]; then
    if [ -f "$DEFAULT_DISK" ]; then
        echo "=== Removing existing system disk ==="
        rm -f "$DEFAULT_DISK"
    fi
fi

if [ ! -f "$DISK" ]; then
    if [ -n "$DISK_PATH" ]; then
        echo "Error: specified disk not found: $DISK_PATH" >&2
        exit 1
    fi
    echo "=== Creating blank system disk ($DISK_SIZE_MB MB) at $DEFAULT_DISK ==="
    mkdir -p "$(dirname "$DEFAULT_DISK")"
    dd if=/dev/zero of="$DEFAULT_DISK" bs=1M count="$DISK_SIZE_MB" status=none
    echo "=== Disk created ==="
    echo ""
fi

# --- Step 3: Boot ---

echo "=== Booting OVMX (${MEMORY} RAM, disk: $DISK) ==="
echo "=== Ctrl-A X to quit QEMU ==="
echo ""

DISK="$DISK" MEMORY="$MEMORY" exec "$SCRIPT_DIR/distro/boot/run-qemu.sh" "$KERNEL" "$INITRD"
