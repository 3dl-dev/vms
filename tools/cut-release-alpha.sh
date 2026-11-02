#!/bin/bash
# cut-release-alpha.sh - build the Alpha (OVMX/Linux-Alpha, LP64) userspace
# release artifact set for an OVMX release bundle (rd vms-233b, epic vms-f10,
# "co-release parity across aarch64/x86_64/alpha/VAX").
#
# WHAT THIS IS. Alpha (the OVMX/Linux-Alpha substrate -- operator ruling
# 2026-08-16, the FOURTH Linux arch, vms.ko-as-executive recompile) is a
# first-class co-release platform, exactly like VAX. This script is the Alpha
# analog of tools/cut-release-vax.sh: it reuses the SAME alpha-linux-gnu cross
# toolchain (tools/cross-alpha/Dockerfile) and CMake toolchain file
# (tools/cross-alpha/toolchain-alpha-linux.cmake) that ci.yml's alpha-boot-login
# gate and tools/cross-alpha/build-alpha-bootimage.sh already drive, to produce a
# real EM_ALPHA/LP64 artifact set and drop it into a release bundle. It does not
# reinvent the cross-compile; it drives it -- the same way tools/cut-release.sh
# drives distro/Dockerfile.bootable for x86_64 and tools/cut-release-vax.sh
# drives the elf32-vax build.
#
# SCOPE (co-release WIRING gate, vms-233b): the DETERMINISTIC, byte-reproducible
# userspace image set only --
#
#   [every image in the ovmx-images aggregate] the full shipped userspace image
#                      set (STARTUP.EXE, PROVISION.EXE, JOB_CONTROL.EXE, DCL.EXE,
#                      LOGINOUT.EXE, HELP.EXE, AUTHORIZE.EXE, MAIL.EXE,
#                      MONITOR.EXE, INITIALIZE.EXE, INSTALL.EXE, SYSGEN.EXE,
#                      PRODUCT.EXE, LIBRARIAN.EXE, ANALYZE.EXE, SYSMAN.EXE,
#                      CNXTRACE.EXE, PARTS.EXE as of this writing -- CMakeLists.txt's
#                      `_OVMX_IMAGES_DEPS` is authoritative, not this comment),
#                      cross-built EM_ALPHA under the alpha-linux-gnu toolchain
#                      (Rung A of the OVMX-on-Alpha epic, the same images
#                      alpha-boot-login boots). This is the LP64 userland the
#                      GCC lane's Alpha C-RTL (DECC$SHR/LIBOTS$SHR) lands
#                      alongside, so protecting THIS build is exactly what the
#                      co-release gate exists to do for an Alpha-carrying cut.
#
#   vms.ko           the Alpha executive module (rd vms-ec1; vmsfs.ko was retired,
#                      vms-165 -- the Files-11 ACP lives inside vms.ko). Cross-built
#                      by tools/cross-alpha/release-vmsko.sh against a sha256-pinned
#                      Linux/Alpha 6.6.52 tree with the clock/user/host pinned from
#                      SOURCE_DATE_EPOCH and the module built from a fixed path and
#                      debug-stripped. The Alpha kernel config has no MODULE_SIG, so
#                      the module is unsigned: no signature to make deterministic.
#                      Byte-reproducibility is PROVEN, not assumed: the
#                      cut-release-reproducible job cuts twice (--no-cache) and
#                      cmp's every alpha/ artifact, vms.ko included; the
#                      cut-release-alpha-gate job fails if vms.ko is absent.
#
#   vmlinux-boot     the Alpha BOOTABLE set (rd vms-f10): the Linux/Alpha kernel
#   ovmx-distrib-alpha.img  with the bootstrap initramfs (STARTUP.EXE as /init,
#                      vms.ko) baked in, and the genuine ODS-2 system disk -- the
#                      SAME assembly alpha-boot-login boots
#                      (tools/cross-alpha/build-alpha-bootimage.sh), run in its
#                      OVMX_ALPHA_RELEASE product-disk mode under SOURCE_DATE_EPOCH.
#                      release-acceptance-e2e boots them from the cut bundle to an
#                      authenticated DCL prompt, runs PARTS and checks the version.
#
# This script writes $OUT_DIR/alpha-artifact-manifest.txt: the flat, newline-
# separated list of every artifact basename it actually shipped, in CMake's own
# install order. tools/cut-release.sh reads THAT file to build its
# ALPHA_ARTIFACT_ORDER array -- the shipped set is generated fresh on every cut
# from install_manifest_ovmx-images.txt, never hand-duplicated, so there is
# nothing left to drift (the same DERIVED-manifest discipline vms-88c gave VAX).
#
# GATE SEMANTICS (the co-release invariant, made mechanical). This script runs
# under `set -e` and exits nonzero on ANY stage failure. tools/cut-release.sh
# calls it UNCONDITIONALLY -- there is no --skip-alpha escape hatch -- so a
# broken Alpha build fails the release cut itself. Every caller of cut-release.sh
# (cut-release-reproducible, release-acceptance-e2e, release.yml's tag-triggered
# cut-and-publish, `make release`, and a conductor cutting by hand) inherits the
# Alpha gate for free, with no separate wiring to keep in sync.
#
# OVMX_ALPHA_RELEASE_NEGCTL=1 deliberately fails the userspace-image stage -- a
# controlled proof that the GATING plumbing here propagates an Alpha build
# failure into a nonzero exit (the "a release is NOT cuttable if the Alpha build
# fails" half of the invariant, proven mechanically, mirroring VAX's
# OVMX_VAX_RELEASE_NEGCTL). Never set this in a real cut.
#
# Usage:
#   tools/cut-release-alpha.sh --src-dir DIR --out-dir DIR [options]
#
# Options:
#   --src-dir DIR              Source tree to build from (required; the same
#                               archived/checked-out tree cut-release.sh is
#                               cutting -- never the live working repo, Rule 9's
#                               "never rw-mount + build into the repo")
#   --out-dir DIR              Destination for the flat Alpha artifact set
#                               (required)
#   --toolchain-image NAME     Tag for the built ovmx-cross-alpha image
#                               (default: ovmx-cross-alpha:release)
#   --source-date-epoch N      Reproducible-build clock for vms.ko (required)
#   --no-cache                 Force a fresh (non-layer-cached) build of the
#                               cross toolchain image
#   -h|--help                  Show this help and exit
#
# Exit 0 = the full Alpha userspace artifact set + vms.ko + the bootable set was built and copied into
# --out-dir.

set -euo pipefail

SRC_DIR=""
OUT_DIR=""
TOOLCHAIN_IMAGE="ovmx-cross-alpha:release"
NO_CACHE=0
SDE=""

usage() {
    sed -n '2,95p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --src-dir) SRC_DIR="$2"; shift 2 ;;
        --src-dir=*) SRC_DIR="${1#--src-dir=}"; shift ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --out-dir=*) OUT_DIR="${1#--out-dir=}"; shift ;;
        --toolchain-image) TOOLCHAIN_IMAGE="$2"; shift 2 ;;
        --toolchain-image=*) TOOLCHAIN_IMAGE="${1#--toolchain-image=}"; shift ;;
        --source-date-epoch) SDE="$2"; shift 2 ;;
        --source-date-epoch=*) SDE="${1#--source-date-epoch=}"; shift ;;
        --no-cache) NO_CACHE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "cut-release-alpha.sh: unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

log() { echo "cut-release-alpha: $*"; }
fail() { echo "cut-release-alpha: FATAL: $*" >&2; exit 1; }

[ -n "$SRC_DIR" ] || fail "--src-dir is required"
[ -n "$OUT_DIR" ] || fail "--out-dir is required"
case "$SDE" in ''|*[!0-9]*) fail "--source-date-epoch must be a nonnegative integer, got: '$SDE'" ;; esac
[ -d "$SRC_DIR" ] || fail "--src-dir does not exist: $SRC_DIR"
[ -f "$SRC_DIR/tools/cross-alpha/Dockerfile" ] || fail "not an OVMX tree with Alpha support (no tools/cross-alpha/Dockerfile): $SRC_DIR"
[ -f "$SRC_DIR/tools/cross-alpha/toolchain-alpha-linux.cmake" ] || fail "missing tools/cross-alpha/toolchain-alpha-linux.cmake in $SRC_DIR"

command -v docker >/dev/null 2>&1 || fail "docker not found"

NEGCTL="${OVMX_ALPHA_RELEASE_NEGCTL:-0}"
[ "$NEGCTL" = "1" ] && log "OVMX_ALPHA_RELEASE_NEGCTL=1 -- WILL deliberately fail the userspace-image stage (gate-wiring proof, never set this in a real cut)"

mkdir -p "$OUT_DIR"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/ovmx-cut-release-alpha.XXXXXX")"
cleanup() {
    # Every docker-run stage below writes into $WORK as container-root, so a
    # plain host `rm -rf` can hit "Permission denied" on root-owned nested dirs.
    # Clear the CONTENTS from inside a container (as root, same image already
    # pulled) first; $WORK itself is then empty and host-owned, so rmdir finishes.
    if [ -d "$WORK" ]; then
        if docker image inspect "$TOOLCHAIN_IMAGE" >/dev/null 2>&1; then
            docker run --rm -v "$WORK:/cleanup" "$TOOLCHAIN_IMAGE" \
                sh -c 'rm -rf /cleanup/*' >/dev/null 2>&1 || true
        fi
        rm -rf "$WORK" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# --- Toolchain image (layer-cached locally across repeated invocations) -------
# The same ovmx-cross-alpha image ci.yml's alpha-boot-login gate builds
# (alpha-linux-gnu cross toolchain). Tagging it :release keeps a hand cut from
# clobbering a developer's cached :latest.
log "building the alpha-linux-gnu cross toolchain image ($TOOLCHAIN_IMAGE)"
BUILD_ARGS=(build -t "$TOOLCHAIN_IMAGE" -f "$SRC_DIR/tools/cross-alpha/Dockerfile")
[ "$NO_CACHE" -eq 1 ] && BUILD_ARGS+=(--no-cache)
BUILD_ARGS+=("$SRC_DIR/tools/cross-alpha")
docker "${BUILD_ARGS[@]}"

# --- Userspace images (ovmx-images CMake aggregate, EM_ALPHA/LP64) ------------
# Also the OVMX_ALPHA_RELEASE_NEGCTL target -- fail loudly BEFORE spending the
# build time, the same pattern cut-release-vax.sh's stage-3 negctl uses.
log "=== stage 1/3: userspace images (ovmx-images CMake aggregate, EM_ALPHA) ==="
if [ "$NEGCTL" = "1" ]; then
    fail "OVMX_ALPHA_RELEASE_NEGCTL: deliberately failing the userspace-image stage to prove the release-cut gate has teeth against a failed Alpha build"
fi

# STAGE is bind-mounted at /out; /out/build is the CMake build tree, /out/stage
# is the --install destination prefix -- both subdirectories of the mount point,
# mirroring cut-release-vax.sh's stage-3 convention. Configure flags match
# tools/cross-alpha/build-alpha-bootimage.sh's userland build (static, tools ON,
# tests off) so the shipped images are byte-identical to what alpha-boot-login
# actually boots.
STAGE="$WORK/images"; mkdir -p "$STAGE/build" "$STAGE/stage"
docker run --rm \
    -v "$SRC_DIR:/src:ro" -w /src \
    -v "$STAGE:/out" \
    "$TOOLCHAIN_IMAGE" sh -c '
        set -eu
        cmake -S /src -B /out/build \
            -DCMAKE_TOOLCHAIN_FILE=/src/tools/cross-alpha/toolchain-alpha-linux.cmake \
            -DCMAKE_BUILD_TYPE=Release \
            -DBUILD_TESTS=OFF -DBUILD_TOOLS=ON -DOVMX_STATIC=ON \
            -DCMAKE_INSTALL_PREFIX=/out/stage
        cmake --build /out/build --target ovmx-images -- -j"$(nproc)"
        cmake --install /out/build --component ovmx-images
    '

# install_manifest_ovmx-images.txt (CMake writes this name because the install
# above named an explicit --component; see CMakeLists.txt's COMPONENT ovmx-images
# install loop, vms-88c) records every file that COMPONENT installed, as absolute
# paths INSIDE the container (under /out/stage/...). $STAGE is the same bind
# mount, so the host path is the container path with the /out prefix swapped for
# $STAGE -- no separate translation table to maintain.
IMAGE_MANIFEST="$STAGE/build/install_manifest_ovmx-images.txt"
[ -f "$IMAGE_MANIFEST" ] || fail "install_manifest_ovmx-images.txt missing after cmake --install --component ovmx-images -- the ovmx-images CMakeLists.txt COMPONENT install loop did not run under the alpha toolchain"
[ -s "$IMAGE_MANIFEST" ] || fail "install_manifest_ovmx-images.txt is empty -- ovmx-images installed nothing"

IMAGE_NAMES=()
# `|| [ -n "$container_path" ]` is load-bearing: CMake's own install_manifest_*.txt
# ends WITHOUT a trailing newline after the last entry, so a plain `while read`
# silently drops that last line (see cut-release-vax.sh's identical guard -- the
# self-inflicted silent-drop class INV-6 exists to kill).
while IFS= read -r container_path || [ -n "$container_path" ]; do
    [ -n "$container_path" ] || continue
    name="$(basename "$container_path")"
    host_path="$STAGE${container_path#/out}"
    [ -f "$host_path" ] || fail "image named in install_manifest_ovmx-images.txt is missing on disk: $name ($host_path)"
    cp "$host_path" "$OUT_DIR/$name"
    log "-> $OUT_DIR/$name"
    IMAGE_NAMES+=("$name")
done < "$IMAGE_MANIFEST"
[ "${#IMAGE_NAMES[@]}" -gt 0 ] || fail "no images copied from install_manifest_ovmx-images.txt"
log "ovmx-images aggregate: ${#IMAGE_NAMES[@]} images shipped (derived from install_manifest_ovmx-images.txt, not a hand list)"

# --- Executive module vms.ko (rd vms-ec1) -------------------------------------
log "=== stage 2/3: vms.ko (Linux/Alpha kernel + OVMX executive, SOURCE_DATE_EPOCH=$SDE) ==="
KOUT="$WORK/vmsko"; mkdir -p "$KOUT"
docker run --rm -e "SDE=$SDE" \
    -v "$SRC_DIR:/src:ro" -v "$KOUT:/out" \
    "$TOOLCHAIN_IMAGE" sh /src/tools/cross-alpha/release-vmsko.sh
[ -s "$KOUT/vms.ko" ] || fail "vms.ko missing after the executive-module stage"
cp "$KOUT/vms.ko" "$OUT_DIR/vms.ko"
log "-> $OUT_DIR/vms.ko"
IMAGE_NAMES+=("vms.ko")

# --- Bootable set (rd vms-f10): vmlinux-boot + ovmx-distrib-alpha.img ----------
# The SAME assembly alpha-boot-login / run-boot-alpha.sh boot (tools/cross-alpha/
# build-alpha-bootimage.sh, from the archived tree), in its release mode: the
# product disk (no harness payload) with every embedded clock pinned from
# SOURCE_DATE_EPOCH. vmlinux-boot is the Linux/Alpha kernel with the bootstrap
# initramfs (STARTUP.EXE as /init, vms.ko, SYSUAF/SYSMGR) baked in; the disk is
# the genuine ODS-2 system volume. release-acceptance-e2e boots exactly these two
# files; cut-release-reproducible cmp's them across two cuts. A failed assembly
# or a missing file fails the cut.
# HISTORICAL-BASELINE EXCEPTION: a tree whose build-alpha-bootimage.sh predates
# the release mode (no OVMX_ALPHA_RELEASE) cannot assemble a product disk, so it
# ships no bootable set (%CUT-ALPHA-I-PREBOOTSET), as pre-Alpha trees ship none.
BOOTIMG="$SRC_DIR/tools/cross-alpha/build-alpha-bootimage.sh"
if grep -q 'OVMX_ALPHA_RELEASE' "$BOOTIMG" 2>/dev/null; then
    log "=== stage 3/3: bootable set (vmlinux-boot + ovmx-distrib-alpha.img, SOURCE_DATE_EPOCH=$SDE) ==="
    BS="$WORK/bootset"; mkdir -p "$BS"
    ALPHA_IMG="$TOOLCHAIN_IMAGE" OVMX_ALPHA_RELEASE=1 SOURCE_DATE_EPOCH="$SDE" FORCE_BUILD=1 \
        VMSKO_WORK="$BS/vmsko" USERLAND="$BS/userland" WORK="$BS/boot" \
        bash "$BOOTIMG" || fail "Alpha bootable-set assembly (build-alpha-bootimage.sh) failed"
    for f in vmlinux-boot ovmx-distrib-alpha.img; do
        [ -s "$BS/boot/$f" ] || fail "Alpha bootable set incomplete: $f missing after build-alpha-bootimage.sh"
        cp "$BS/boot/$f" "$OUT_DIR/$f"
        log "-> $OUT_DIR/$f"
        IMAGE_NAMES+=("$f")
    done
    file "$OUT_DIR/vmlinux-boot" | grep -qi 'alpha' || fail "vmlinux-boot is not an Alpha ELF kernel"
else
    log "%CUT-ALPHA-I-PREBOOTSET: this tree predates the Alpha bootable release set (no OVMX_ALPHA_RELEASE in build-alpha-bootimage.sh) -- no bootable set for this historical cut"
fi

# --- Shipped-artifact manifest: the DERIVED "what actually shipped" list, every
# image name from the CMake install manifest, in the order CMake installed them.
# tools/cut-release.sh reads this to build its ALPHA_ARTIFACT_ORDER array --
# generated fresh every cut, never hand-duplicated, so it cannot drift from what
# was actually built.
printf '%s\n' "${IMAGE_NAMES[@]}" > "$OUT_DIR/alpha-artifact-manifest.txt"
log "-> $OUT_DIR/alpha-artifact-manifest.txt (${#IMAGE_NAMES[@]} artifacts total)"

log "=== ALL ALPHA RELEASE ARTIFACTS BUILT: $OUT_DIR ==="
ls -la "$OUT_DIR"
