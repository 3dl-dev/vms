#!/bin/bash
# Boot OVMX in QEMU — everything runs inside Docker, no host dependencies.
#
# Usage:
#   ./boot.sh           # build (if needed) and boot
#   ./boot.sh --rebuild # force rebuild, then boot
#
# Environment:
#   MEMORY  - Guest RAM (default: 512M)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGE="ovmx-boot"
MEMORY="${MEMORY:-512M}"

BUILD_ARGS=""
if [ "$1" = "--rebuild" ] || [ "$1" = "-r" ]; then
    BUILD_ARGS="--no-cache"
fi

# Build if image doesn't exist or rebuild requested
if [ -n "$BUILD_ARGS" ] || ! docker image inspect "$IMAGE" &>/dev/null; then
    echo "=== Building OVMX bootable image ==="
    docker build -f "$SCRIPT_DIR/distro/Dockerfile.bootable" \
        -t "$IMAGE" $BUILD_ARGS "$SCRIPT_DIR"
    echo "=== Build complete ==="
    echo ""
fi

echo "=== Booting OVMX (${MEMORY} RAM, Ctrl-A X to quit) ==="
exec docker run --rm -it -e MEMORY="$MEMORY" "$IMAGE"
