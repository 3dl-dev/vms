#!/bin/bash
# run_upgrade_e2e.sh - ctest entry point for upgrade_e2e (vms-f05). Same
# convention as run_product_install_e2e.sh (read that file's header for the
# shared reasoning): registered with ctest so it is discoverable by name,
# gated behind an explicit opt-in so a bare `ctest` run never pays for a
# real docker+QEMU boot it cannot use.
#
# THIS TEST NEEDS TWO REAL RELEASE BUNDLES (a BASELINE and an UPGRADE, cut
# by tools/cut-release.sh from two different commits with two different
# OVMX_PRODUCT_VERSION values -- vms-f05's own constraint: never hand-fake
# "0.N"/"0.N+1"). Cutting them is a ~25-30 minute container build EACH
# (tools/cut-release.sh's own estimate), so this script does not cut them
# itself -- it requires them prebuilt and points at them by directory,
# exactly like run_product_install_e2e.sh requires a prebuilt
# INITIALIZE.EXE rather than building the whole host toolchain inline.
# See .github/workflows/ci.yml's upgrade-e2e job for how CI produces both.
#
# Local reproduction:
#   tools/cut-release.sh --ref <baseline-commit> --out-dir /tmp/rel-baseline
#   tools/cut-release.sh --ref HEAD              --out-dir /tmp/rel-upgrade
#   cmake -B build -DBUILD_TOOLS=ON && cmake --build build \
#       --target vms_initialize vmsfs_master
#   OVMX_QEMU_FULL_E2E=1 OVMX_BUILD_BOOT_IMAGE=1 \
#   OVMX_BASELINE_RELEASE_DIR=/tmp/rel-baseline \
#   OVMX_UPGRADE_RELEASE_DIR=/tmp/rel-upgrade \
#       tests/qemu/run_upgrade_e2e.sh
#
# Env knobs:
#   OVMX_QEMU_FULL_E2E        must be "1" or this script SKIPs (exit 77).
#   OVMX_BASELINE_RELEASE_DIR path to a cut-release.sh bundle (older version).
#   OVMX_UPGRADE_RELEASE_DIR  path to a cut-release.sh bundle (newer version).
#   OVMX_BOOT_IMAGE           image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE     "1" to build OVMX_BOOT_IMAGE if not already
#                             present (default: 0 - require a prebuilt image).
#                             Only used for its qemu-system-x86_64 binary and
#                             general boot tooling -- this test boots the
#                             UPGRADE bundle's OWN kernel/initrd/distrib.img,
#                             never the image's baked-in ones.
#   OVMX_VMSFS_MASTER         path to a host-built vmsfs_master (default:
#                             searches ../../build*/bin/vmsfs_master).
#   OVMX_INITIALIZE_EXE       path to a host-built INITIALIZE.EXE (default:
#                             searches ../../build*/bin/INITIALIZE.EXE).
#   BOOT_TIMEOUT / RUN_TIMEOUT   forwarded to the gate itself.

set -uo pipefail

SKIP=77
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: upgrade_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot, real minutes)."
    echo "      See this script's header for the two-cut-release local reproduction recipe."
    exit "$SKIP"
fi

command -v docker >/dev/null 2>&1 || { echo "SKIP: docker not available"; exit "$SKIP"; }

BASELINE_DIR="${OVMX_BASELINE_RELEASE_DIR:-}"
UPGRADE_DIR="${OVMX_UPGRADE_RELEASE_DIR:-}"
if [ -z "$BASELINE_DIR" ] || [ -z "$UPGRADE_DIR" ]; then
    echo "SKIP: upgrade_e2e requires OVMX_BASELINE_RELEASE_DIR and OVMX_UPGRADE_RELEASE_DIR"
    echo "      (two tools/cut-release.sh bundles). See this script's header for the recipe."
    exit "$SKIP"
fi
for f in vmlinuz initramfs-ovmx-slim.cpio.gz ovmx-distrib.img ovmx-os.kit; do
    [ -f "$BASELINE_DIR/$f" ] || { echo "FATAL: $BASELINE_DIR/$f missing"; exit 1; }
    [ -f "$UPGRADE_DIR/$f" ]  || { echo "FATAL: $UPGRADE_DIR/$f missing"; exit 1; }
done

# The versions the test asserts against come from each cut's OWN
# release-manifest.json (written by tools/cut-release.sh from the
# archived commit's ovmx_identity.h, INV-1) -- never a literal in this
# script or in test_upgrade_e2e.sh. This is what lets the PINNED baseline
# commit (28a929b2, see test_upgrade_e2e.sh's header) keep working
# unmodified across every future OVMX_PRODUCT_VERSION bump: the UPGRADE
# side is "this workflow's own commit (HEAD)", whatever version that is.
manifest_version() {
    grep '"product_version"' "$1/release-manifest.json" | head -1 \
        | sed -n 's/.*"product_version": "\([^"]*\)".*/\1/p'
}
EXPECTED_BASELINE_VERSION="$(manifest_version "$BASELINE_DIR")"
EXPECTED_UPGRADE_VERSION="$(manifest_version "$UPGRADE_DIR")"
[ -n "$EXPECTED_BASELINE_VERSION" ] || { echo "FATAL: could not read product_version from $BASELINE_DIR/release-manifest.json"; exit 1; }
[ -n "$EXPECTED_UPGRADE_VERSION" ]  || { echo "FATAL: could not read product_version from $UPGRADE_DIR/release-manifest.json"; exit 1; }
if [ "$EXPECTED_BASELINE_VERSION" = "$EXPECTED_UPGRADE_VERSION" ]; then
    echo "FATAL: baseline and upgrade cuts report the SAME product_version ($EXPECTED_BASELINE_VERSION) -- the upgrade assertion would be vacuous."
    exit 1
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    if [ "${OVMX_BUILD_BOOT_IMAGE:-0}" = "1" ]; then
        echo "--- building bootable image ($IMAGE) ---"
        docker build -t "$IMAGE" -f "$REPO_ROOT/distro/Dockerfile.bootable" "$REPO_ROOT" \
            || { echo "FAIL: bootable image build"; exit 1; }
    else
        echo "FATAL: image '$IMAGE' not found. Either:"
        echo "  docker build -t $IMAGE -f distro/Dockerfile.bootable ."
        echo "or re-run with OVMX_BUILD_BOOT_IMAGE=1 to build it here."
        exit 1
    fi
fi

# Locate host-built INITIALIZE.EXE and vmsfs_master -- both pure userspace
# tools (no vmsfs.ko, no kernel module load on the host; Project Rule 6).
INIT_EXE="${OVMX_INITIALIZE_EXE:-}"
if [ -z "$INIT_EXE" ]; then
    for candidate in "$REPO_ROOT"/build/bin/INITIALIZE.EXE "$REPO_ROOT"/build-static/bin/INITIALIZE.EXE; do
        if [ -x "$candidate" ]; then INIT_EXE="$candidate"; break; fi
    done
fi
if [ -z "$INIT_EXE" ] || [ ! -x "$INIT_EXE" ]; then
    echo "SKIP: no host-built INITIALIZE.EXE found (build one: cmake -B build -DBUILD_TOOLS=ON && cmake --build build --target vms_initialize)."
    echo "      Set OVMX_INITIALIZE_EXE=/path/to/INITIALIZE.EXE to point at one directly."
    exit "$SKIP"
fi

MASTER_EXE="${OVMX_VMSFS_MASTER:-}"
if [ -z "$MASTER_EXE" ]; then
    for candidate in "$REPO_ROOT"/build/bin/vmsfs_master "$REPO_ROOT"/build-static/bin/vmsfs_master; do
        if [ -x "$candidate" ]; then MASTER_EXE="$candidate"; break; fi
    done
fi
if [ -z "$MASTER_EXE" ] || [ ! -x "$MASTER_EXE" ]; then
    echo "SKIP: no host-built vmsfs_master found (build one: cmake -B build -DBUILD_TOOLS=ON && cmake --build build --target vmsfs_master)."
    echo "      Set OVMX_VMSFS_MASTER=/path/to/vmsfs_master to point at one directly."
    exit "$SKIP"
fi

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

echo "--- formatting the blank upgrade target (DKA100:, label WORK) with $INIT_EXE ---"
# --ods2: the runtime MOUNTs a GENUINE ODS-2 volume over the ACP (atomic flip,
# vms-208); INITIALIZE's default legacy vmsfs layout is refused (%OVMX-F-
# MOUNTFAIL). A real upgrade target is an ODS-2 volume -- format one (vms-37e).
"$INIT_EXE" --ods2 "$WORKDIR/dka100.img" WORK 16 || { echo "FAIL: INITIALIZE of the target disk"; exit 1; }

echo "--- mastering the kit-carrier disk (DKA200:, label KITS) with $MASTER_EXE ---"
KITSTAGE="$WORKDIR/kitstage/SYSUPD"
mkdir -p "$KITSTAGE"
cp "$BASELINE_DIR/ovmx-os.kit" "$KITSTAGE/OVMX-OS-BASELINE.KIT"
cp "$UPGRADE_DIR/ovmx-os.kit"  "$KITSTAGE/OVMX-OS-UPGRADE.KIT"
# --ods2: the runtime MOUNTs GENUINE ODS-2 volumes over the ACP only (atomic
# flip, vms-208) -- a legacy vmsfs layout is refused (%OVMX-F-MOUNTFAIL,
# "DKA200: would not mount as ODS-2"), exactly as the DKA100 target above notes.
# The kit carrier is a real mounted disk the guest reads OVMX-OS-*.KIT off of,
# so it must be a genuine ODS-2 volume just like DKA100:, not the master tool's
# default legacy layout.
"$MASTER_EXE" --ods2 master "$WORKDIR/dka200.img" KITS "$WORKDIR/kitstage" \
    || { echo "FAIL: mastering the kit-carrier disk"; exit 1; }

exec docker run --rm \
    -e "BOOT_TIMEOUT=${BOOT_TIMEOUT:-90}" \
    -e "RUN_TIMEOUT=${RUN_TIMEOUT:-90}" \
    -e "EXPECTED_BASELINE_VERSION=${EXPECTED_BASELINE_VERSION}" \
    -e "EXPECTED_UPGRADE_VERSION=${EXPECTED_UPGRADE_VERSION}" \
    -v "$REPO_ROOT/tests/qemu/test_upgrade_e2e.sh:/test.sh:ro" \
    -v "$UPGRADE_DIR:/upgrade-release:ro" \
    -v "$WORKDIR:/work" \
    --entrypoint bash \
    "$IMAGE" \
    /test.sh
