#!/bin/bash
# ONE command: clean-install OVMX onto a fresh system disk, then boot it
# interactively to a SYSTEM login / DCL prompt.
#
# Usage:
#   ./boot.sh                          # Build (if needed), clean install + boot
#   ./boot.sh --clean                  # Force a fresh disk (delete and reinstall)
#   ./boot.sh --disk path/to/disk.img  # Boot with specified disk image
#   ./boot.sh --rebuild                # Force Docker image rebuild, then boot
#   ./boot.sh --slim                   # Use slim initramfs (needs installed disk)
#   ./boot.sh --distrib                # Boot the PRE-INSTALLED distribution disk
#                                      #   (dist/ovmx-distrib.img, mastered at build;
#                                      #    slim initramfs, no self-install — vms-8ab)
#   ./boot.sh --flags R5,R6            # Conversational boot (vms-b81): halt at
#                                      #   SYSBOOT> before the executive attaches.
#                                      #   Bit 0 of R6 is the conversational bit --
#                                      #   --flags 0,1 matches the oracle's own
#                                      #   `boot -flags 0,1` example.
#   ./boot.sh --help                   # Show this help
#
# First run (or --clean): Docker builds the image if needed, the container
#             creates a blank system disk, and this SAME boot installs OVMX
#             onto it (INITIALIZE.EXE + system seed) and continues straight
#             into a login prompt — one continuous QEMU session.
# Later runs (no flags):  Reuses the existing installed disk — boots
#             immediately, state persists across runs.
#
# At the Username: prompt, log in as SYSTEM / MANAGER for a full-privilege
# DCL session (or GUEST / GUEST for a restricted one).
#
# QEMU runs inside Docker — no host QEMU install needed.
# The disk image is volume-mounted so writes persist on the host.
# To exit QEMU: Ctrl-A then X.
#
# Environment:
#   MEMORY  - Guest RAM (default: 512M)
#
# --fresh is accepted as a deprecated alias for --clean.

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
USE_DISTRIB=0
DISK_PATH=""
BOOT_FLAGS=""

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --rebuild|-r)
            FORCE_REBUILD=1
            shift
            ;;
        --clean|--fresh|-f)
            FORCE_FRESH=1
            shift
            ;;
        --slim|-s)
            USE_SLIM=1
            shift
            ;;
        --distrib)
            USE_DISTRIB=1
            shift
            ;;
        --flags)
            if [ -z "$2" ]; then
                echo "Error: --flags requires an R5,R6 argument" >&2
                exit 1
            fi
            BOOT_FLAGS="$2"
            shift 2
            ;;
        --flags=*)
            BOOT_FLAGS="${1#--flags=}"
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
            sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--clean] [--rebuild] [--slim] [--distrib] [--disk path] [--flags R5,R6]" >&2
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
    echo "=== Building image (compiles kernel modules + VMS-native toolchain + QEMU; ~25-30 min on a cold cache)... ==="
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

CLEAN_INSTALL=0
if [ "$FORCE_FRESH" -eq 1 ] && [ -z "$DISK_PATH" ]; then
    if [ -f "$DISK" ]; then
        echo "=== Creating clean system disk (removing existing $DISK_NAME)... ==="
        rm -f "$DISK"
    fi
    CLEAN_INSTALL=1
elif [ ! -f "$DISK" ]; then
    echo "=== Creating clean system disk ($DISK_NAME does not exist yet)... ==="
    CLEAN_INSTALL=1
fi

# --- Step 3: Boot via Docker ---

INITRD_ENV="fat"
[ "$USE_SLIM" -eq 1 ] && INITRD_ENV="slim"
# --distrib forces the slim initramfs and a pre-installed disk (see below).
[ "$USE_DISTRIB" -eq 1 ] && INITRD_ENV="slim"

if [ "$USE_DISTRIB" -eq 1 ]; then
    if [ "$CLEAN_INSTALL" -eq 1 ]; then
        echo "=== Booting — seeding disk from the PRE-INSTALLED distribution image (dist/ovmx-distrib.img), then logging in (${MEMORY} RAM, initrd: slim) ==="
    else
        echo "=== Booting — log in as SYSTEM (${MEMORY} RAM, disk: $DISK_NAME, initrd: slim, pre-installed distribution disk) ==="
    fi
elif [ "$CLEAN_INSTALL" -eq 1 ]; then
    echo "=== Booting — first boot installs OVMX (INITIALIZE + system seed), then logs in (${MEMORY} RAM, initrd: $INITRD_ENV) ==="
else
    echo "=== Booting — log in as SYSTEM (${MEMORY} RAM, disk: $DISK_NAME, initrd: $INITRD_ENV) ==="
fi
echo "=== Username: SYSTEM   Password: MANAGER   (or GUEST / GUEST for a restricted session) ==="
echo "=== To exit QEMU: Ctrl-A then X ==="
echo ""

exec docker run --rm -it \
    -e MEMORY="$MEMORY" \
    -e INITRD="$INITRD_ENV" \
    -e DISTRIB="$([ "$USE_DISTRIB" -eq 1 ] && echo 1)" \
    -e BOOT_FLAGS="$BOOT_FLAGS" \
    -e SYSDISK_NAME="$DISK_NAME" \
    -v "$DISK_DIR:/data" \
    "$IMAGE"
