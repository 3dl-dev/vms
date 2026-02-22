#!/bin/bash
# Boot OVMX in QEMU with persistent system disk.
#
# Usage:
#   ./boot.sh                          # Build (if needed), boot with default disk
#   ./boot.sh --disk path/to/disk.img  # Boot with specified disk image
#   ./boot.sh --fresh                  # Force fresh disk (delete and recreate)
#   ./boot.sh --rebuild                # Force Docker rebuild, then boot
#   ./boot.sh --slim                   # Use slim initramfs (needs installed disk)
#
# First run:  Docker builds the image, creates blank dist/sysdisk.img,
#             boots → STARTUP.EXE initializes disk and installs.
# Later runs: Detects image + disk exist, boots immediately → fast.
#
# QEMU runs inside Docker — no host QEMU install needed.
# The disk image is volume-mounted so writes persist on the host.
#
# Environment:
#   MEMORY  - Guest RAM (default: 512M)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DIST_DIR="$SCRIPT_DIR/dist"
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
            sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
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

# --- Step 1: Build Docker image if needed ---
#
# Rebuild when:
#   --rebuild flag is set
#   Image doesn't exist
#   Source tree has changed since last build (git tree hash mismatch)
#
# The tree hash of the last build is stored in dist/.build-hash.
# This catches any change to tracked files (source, Dockerfile, configs).

BUILD_HASH_FILE="$DIST_DIR/.build-hash"

current_tree_hash() {
    # Hash of all tracked files — changes when any source file changes.
    # Falls back to "unknown" if not in a git repo.
    git -C "$SCRIPT_DIR" write-tree 2>/dev/null || echo "unknown"
}

need_build=0
if [ "$FORCE_REBUILD" -eq 1 ]; then
    need_build=1
elif ! docker image inspect "$IMAGE" &>/dev/null; then
    need_build=1
else
    CURRENT_HASH=$(current_tree_hash)
    LAST_HASH=""
    [ -f "$BUILD_HASH_FILE" ] && LAST_HASH=$(cat "$BUILD_HASH_FILE")
    if [ "$CURRENT_HASH" != "$LAST_HASH" ]; then
        echo "=== Source tree changed since last build, rebuilding ==="
        need_build=1
    fi
fi

if [ "$need_build" -eq 1 ]; then
    echo "=== Building OVMX bootable image ==="
    BUILD_ARGS=""
    [ "$FORCE_REBUILD" -eq 1 ] && BUILD_ARGS="--no-cache"
    docker build -f "$SCRIPT_DIR/distro/Dockerfile.bootable" \
        -t "$IMAGE" $BUILD_ARGS "$SCRIPT_DIR"
    mkdir -p "$DIST_DIR"
    current_tree_hash > "$BUILD_HASH_FILE"
    echo "=== Build complete ==="
    echo ""
fi

# --- Step 2: Prepare system disk directory ---
#
# We mount a host DIRECTORY into the container rather than a single file.
# This avoids bind-mount permission issues (rootless Docker / user-namespace
# remapping) — the container creates and owns the disk file inside the
# mounted directory, so QEMU can read and write it freely.

DISK_DIR="$(dirname "$DISK")"
DISK_NAME="$(basename "$DISK")"

mkdir -p "$DISK_DIR"

if [ "$FORCE_FRESH" -eq 1 ] && [ -z "$DISK_PATH" ]; then
    if [ -f "$DISK" ]; then
        echo "=== Removing existing system disk ==="
        rm -f "$DISK"
    fi
fi

# --- Step 3: Boot via Docker ---

INITRD_ENV="fat"
[ "$USE_SLIM" -eq 1 ] && INITRD_ENV="slim"

echo "=== Booting OVMX (${MEMORY} RAM, disk: $DISK_NAME, initrd: $INITRD_ENV) ==="
echo "=== Ctrl-A X to quit QEMU ==="
echo ""

exec docker run --rm -it \
    -e MEMORY="$MEMORY" \
    -e INITRD="$INITRD_ENV" \
    -e SYSDISK_NAME="$DISK_NAME" \
    -v "$DISK_DIR:/data" \
    "$IMAGE"
