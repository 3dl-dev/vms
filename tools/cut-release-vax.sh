#!/bin/bash
# cut-release-vax.sh - build the VAX (netbsd-vax / elf32-vax) release artifact
# set for an OVMX release bundle (rd vms-ca5, epic vms-f10, "R2 release
# parity": docs/design-vax-mainstream-release.md).
#
# WHAT THIS IS. VAX is a first-class co-release platform, not a side-thread
# (docs/design-vax-mainstream-release.md). R1 (vms-9172, DONE) made a broken
# VAX build red a PR via fast per-PR cross-compile gates in ci.yml
# (netbsd-vax-vms-crosscompile, vmsdcl-netbsd-
# vax, ovmx-init-netbsd-vax, librarian-netbsd-vax). vms-d65 (unified
# cross-build Rung C) retired the standalone ovmx-boot-images-netbsd-vax
# job -- the boot-image set's per-PR proof now lives in vax-cmake-images'
# activation loop. This script is R2: it reuses that SAME cross toolchain image and
# pinned NetBSD syssrc to produce a real VAX artifact set and drop it into a
# release bundle. It does not reinvent the cross-compile; it drives it, the
# same way tools/cut-release.sh drives distro/Dockerfile.bootable rather than
# reinventing the x86_64 build.
#
# vms-88c (epic vms-509 "unified cross-platform build" Rung F) changed HOW
# the userspace image set is produced and closed a real accountability gap:
# before this rung, this script called four PARTIAL per-image
# tools/cross-vax/build-*.sh scripts and shipped exactly 9 hand-named
# artifacts, while x86_64's Dockerfile.bootable build shipped the FULL
# `ovmx-images` CMake aggregate (~16-18 images, docs/design-unified-cross-
# build.md §3-B). Stage 3 below now builds and installs that SAME aggregate
# under the vax toolchain and SHIPS EVERY IMAGE IT NAMES -- the userspace
# image set here is DERIVED from CMake's own
# install_manifest_ovmx-images.txt, not a hand-maintained list, so a new
# add_executable() joining CMakeLists.txt's `_OVMX_IMAGES_DEPS` auto-appears
# in the next vax release with zero changes to this script. See
# tools/cross-vax/build-ovmx-images-vax-cmake.sh (vms-64a Rung B) for the
# proof that `cmake --build --target ovmx-images` produces a real,
# Decision-A-compliant elf32-vax executable for every one of those images;
# this script reuses that same configure/build recipe and adds
# `cmake --install --component ovmx-images` on top.
#
# SCOPE (R2, explicitly NOT R3/R4). BUILD + artifact production only -- no
# SIMH boot, no custom MODULAR NetBSD/vax kernel build (that ~40-min kernel +
# boot proof is runtime parity, R3 vms-065, gated on the boot-to-DCL capstone
# vms-d59, still in flight). This script produces:
#
#   vms.kmod.o        the OVMX executive module -- a RELOCATABLE object
#                      (build-vms-module-vax.sh). NOT yet a loadable .kmod:
#                      NetBSD/vax GENERIC ships with no module framework, so
#                      loading a vms module needs the custom MODULAR kernel
#                      only the nightly netbsd-vax-devvms/netbsd-vax-boot
#                      jobs build (R3/R4 territory, tools/cross-vax/build-
#                      vax-modular-kernel.sh). Shipping this as vms.kmod.o
#                      rather than relabeling it "vms.kmod" is a deliberate
#                      Rule-8/INV-6 honesty choice -- never claim an artifact
#                      is more built than it is.
#   (vms-165: the loadable ODS-2 VFS module vmsfs.kmod + its vmsfs_mount helper
#    were retired with the vmsfs VFS driver -- SYS$DISK is read through the
#    executive ACP in vms.kmod.o, never a vmsfs mount, so no VFS module ships.)
#   [every image in the ovmx-images aggregate] the full shipped userspace
#                      image set (STARTUP.EXE, PROVISION.EXE, JOB_CONTROL.EXE,
#                      DCL.EXE, LOGINOUT.EXE, OVMXDUMP, LIBRARIAN.EXE,
#                      INSTALL.EXE, PRODUCT.EXE, HELP.EXE, AUTHORIZE.EXE,
#                      MAIL.EXE, MONITOR.EXE, INITIALIZE.EXE, SYSGEN.EXE,
#                      PARTS.EXE as of this writing, plus SCSD.EXE once
#                      vms-838 lands and LINK.EXE if a future rung gives it a
#                      vax role -- CMakeLists.txt's `_OVMX_IMAGES_DEPS` is
#                      authoritative, not this comment), ordinary dynamic
#                      NetBSD ELF32-vax executables activated by ld.elf_so
#                      (Decision A / vms-42d).
#
# This script ALSO writes $OUT_DIR/vax-artifact-manifest.txt: the flat,
# newline-separated list of every artifact basename it actually shipped, in
# the order it shipped them (modules first, then the CMake install manifest's
# own order). tools/cut-release.sh reads THAT file to build its
# VAX_ARTIFACT_ORDER array -- the shipped set is generated fresh on every
# cut, never hand-duplicated, so there is nothing left to drift.
#
# GATE SEMANTICS (the co-release invariant, made mechanical). This script
# runs under `set -e` and exits nonzero on ANY stage failure.
# tools/cut-release.sh calls it UNCONDITIONALLY -- there is no --skip-vax
# escape hatch -- so a broken VAX build fails the release cut itself. That
# makes the invariant mechanical inside the tool, not advisory CI wiring:
# every caller of cut-release.sh (the cut-release-reproducible gate, the
# release-acceptance-e2e gate, release.yml's tag-triggered cut-and-publish,
# `make release`, and a conductor cutting by hand) inherits the VAX gate for
# free, with no separate wiring to keep in sync.
#
# OVMX_VAX_RELEASE_NEGCTL=1 deliberately fails the userspace-image stage --
# a controlled proof that the GATING plumbing here propagates a VAX build
# failure into a nonzero exit, distinct from (and a complement to) R1's own
# per-script "broken TU" teeth checks, which prove the COMPILER rejects bad
# code. Never set this in a real cut.
#
# Usage:
#   tools/cut-release-vax.sh --src-dir DIR --out-dir DIR [options]
#
# Options:
#   --src-dir DIR              Source tree to build from (required; the
#                               same archived/checked-out tree cut-release.sh
#                               is cutting -- never the live working repo,
#                               Rule 9's "never rw-mount + build into the repo")
#   --out-dir DIR               Destination for the flat VAX artifact set
#                               (required)
#   --netbsd-src-cache DIR      Cache dir for the fetched+verified NetBSD
#                               syssrc (default: a stable /tmp path, shared
#                               across repeated invocations on the same
#                               runner so a second cut in the same job -- e.g.
#                               cut-release-reproducible's cut A + cut B --
#                               does not re-download it)
#   --toolchain-image NAME      Tag for the built ovmx-cross-vax image
#                               (default: ovmx-cross-vax:release)
#   --no-cache                  Force a fresh (non-layer-cached) build of the
#                               cross toolchain image
#   -h|--help                   Show this help and exit
#
# Exit 0 = the full VAX artifact set was built and copied into --out-dir.

set -euo pipefail

SRC_DIR=""
OUT_DIR=""
NBSRC_CACHE="${TMPDIR:-/tmp}/ovmx-vax-netbsd-syssrc-cache"
TOOLCHAIN_IMAGE="ovmx-cross-vax:release"
NO_CACHE=0

usage() {
    sed -n '2,113p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --src-dir) SRC_DIR="$2"; shift 2 ;;
        --src-dir=*) SRC_DIR="${1#--src-dir=}"; shift ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --out-dir=*) OUT_DIR="${1#--out-dir=}"; shift ;;
        --netbsd-src-cache) NBSRC_CACHE="$2"; shift 2 ;;
        --netbsd-src-cache=*) NBSRC_CACHE="${1#--netbsd-src-cache=}"; shift ;;
        --toolchain-image) TOOLCHAIN_IMAGE="$2"; shift 2 ;;
        --toolchain-image=*) TOOLCHAIN_IMAGE="${1#--toolchain-image=}"; shift ;;
        --no-cache) NO_CACHE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "cut-release-vax.sh: unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

log() { echo "cut-release-vax: $*"; }
fail() { echo "cut-release-vax: FATAL: $*" >&2; exit 1; }

[ -n "$SRC_DIR" ] || fail "--src-dir is required"
[ -n "$OUT_DIR" ] || fail "--out-dir is required"
[ -d "$SRC_DIR" ] || fail "--src-dir does not exist: $SRC_DIR"
[ -f "$SRC_DIR/tools/cross-vax/Dockerfile" ] || fail "not an OVMX tree (no tools/cross-vax/Dockerfile): $SRC_DIR"

command -v docker >/dev/null 2>&1 || fail "docker not found"

NEGCTL="${OVMX_VAX_RELEASE_NEGCTL:-0}"
[ "$NEGCTL" = "1" ] && log "OVMX_VAX_RELEASE_NEGCTL=1 -- WILL deliberately fail the userspace-image stage (gate-wiring proof, never set this in a real cut)"

mkdir -p "$OUT_DIR"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/ovmx-cut-release-vax.XXXXXX")"
cleanup() {
    # Every docker-run stage below writes into $WORK as container-root, so a
    # plain host `rm -rf` can hit "Permission denied" on root-owned nested
    # dirs (the host user owns $WORK itself, but not everything docker wrote
    # inside it). Clear the CONTENTS from inside a container (as root, same
    # image already pulled) first; $WORK itself is then empty and
    # host-owned, so a plain rmdir finishes the job.
    if [ -d "$WORK" ]; then
        if docker image inspect "$TOOLCHAIN_IMAGE" >/dev/null 2>&1; then
            docker run --rm -v "$WORK:/cleanup" "$TOOLCHAIN_IMAGE" \
                sh -c 'rm -rf /cleanup/*' >/dev/null 2>&1 || true
        fi
        rm -rf "$WORK" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# --- Toolchain image (layer-cached locally across repeated invocations) ----
log "building the vax--netbsdelf cross toolchain image ($TOOLCHAIN_IMAGE)"
BUILD_ARGS=(build -t "$TOOLCHAIN_IMAGE" -f "$SRC_DIR/tools/cross-vax/Dockerfile")
[ "$NO_CACHE" -eq 1 ] && BUILD_ARGS+=(--no-cache)
BUILD_ARGS+=("$SRC_DIR/tools/cross-vax")
docker "${BUILD_ARGS[@]}"

# --- NetBSD/vax kernel headers (syssrc), pinned + checksummed, cached ------
# Same pin the amd64 gate (tests/netbsd/netbsd_version.env) and the vax lab
# harness (tests/lab-vax/run-boot.sh) both use -- read from the ARCHIVED
# tree's own copy so this always builds against the exact commit being cut,
# never a literal duplicated here.
VERSION_ENV="$SRC_DIR/tests/netbsd/netbsd_version.env"
[ -f "$VERSION_ENV" ] || fail "missing $VERSION_ENV -- cannot resolve the pinned NetBSD syssrc"
# shellcheck disable=SC1090
. "$VERSION_ENV"
[ -n "${NETBSD_SYSSRC_URL:-}" ] || fail "NETBSD_SYSSRC_URL not set by $VERSION_ENV"
[ -n "${NETBSD_SYSSRC_SHA512:-}" ] || fail "NETBSD_SYSSRC_SHA512 not set by $VERSION_ENV"

if [ ! -d "$NBSRC_CACHE/usr/src/sys" ]; then
    log "fetching + verifying NetBSD syssrc (cache miss: $NBSRC_CACHE)"
    mkdir -p "$NBSRC_CACHE"
    TGZ="$WORK/syssrc.tgz"
    curl -fSL --retry 3 -o "$TGZ" "$NETBSD_SYSSRC_URL"
    echo "$NETBSD_SYSSRC_SHA512  $TGZ" | sha512sum -c -
    tar xzf "$TGZ" -C "$NBSRC_CACHE" usr/src/sys usr/src/common
else
    log "NetBSD syssrc cache hit: $NBSRC_CACHE"
fi

# --- Stage 1/2: the OVMX executive module (vms.kmod.o, relocatable) --------
# build-vms-module-vax.sh does `rm -rf "$OUT"; mkdir -p "$OUT"` -- OUT is set
# to a SUBDIRECTORY of the bind mount (/out/build), not the mount point
# itself, because `rm -rf` on the mount point of a bind mount fails with
# "Device or resource busy" (Docker cannot unlink the mountpoint inode).
log "=== stage 1/2: OVMX executive module (vms.kmod.o) ==="
STAGE1="$WORK/vms-module"; mkdir -p "$STAGE1"
docker run --rm \
    -v "$SRC_DIR:/src" -w /src \
    -v "$NBSRC_CACHE:/nbsrc:ro" -e NBSRC=/nbsrc \
    -v "$STAGE1:/out" -e OUT=/out/build \
    "$TOOLCHAIN_IMAGE" tools/cross-vax/build-vms-module-vax.sh
[ -f "$STAGE1/build/vms.kmod.o" ] || fail "vms.kmod.o missing after cross-compile"
cp "$STAGE1/build/vms.kmod.o" "$OUT_DIR/vms.kmod.o"
log "-> $OUT_DIR/vms.kmod.o"

# (vms-165: the loadable ODS-2 VFS module stage -- vmsfs.kmod + its vmsfs_mount
# helper, built by the now-deleted build-vmsfs-mount-vax.sh -- was retired with
# the vmsfs VFS driver. SYS$DISK is read through the executive ACP in the vms
# module (vms.kmod.o, staged above), never a vmsfs mount, so no VFS module ships.)

# --- Stage 2/2: the full userspace image set (ovmx-images aggregate) -------
# (rd vms-88c, epic vms-509 Rung F). Also the OVMX_VAX_RELEASE_NEGCTL target
# -- fail loudly BEFORE spending the build time, same pattern the old stage
# 4/LIBRARIAN.EXE negctl used.
log "=== stage 3/3: userspace images (ovmx-images CMake aggregate) ==="
if [ "$NEGCTL" = "1" ]; then
    fail "OVMX_VAX_RELEASE_NEGCTL: deliberately failing the userspace-image stage to prove the release-cut gate has teeth against a failed VAX build"
fi
# STAGE3 is bind-mounted at /out; /out/build is the CMake build tree,
# /out/stage is the --install destination prefix. Both are subdirectories of
# the mount point, not the mount point itself (the stage 1 `rm -rf "$OUT"`
# constraint doesn't apply here since nothing here removes $STAGE3, but the
# subdirectory convention is kept for consistency with the other stages).
STAGE3="$WORK/images"; mkdir -p "$STAGE3/build" "$STAGE3/stage"
docker run --rm \
    -v "$SRC_DIR:/src" -w /src \
    -v "$STAGE3:/out" \
    "$TOOLCHAIN_IMAGE" sh -c '
        set -eu
        cmake -S /src -B /out/build \
            -DCMAKE_TOOLCHAIN_FILE=/src/tools/cross-vax/toolchain-vax-netbsd.cmake \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX=/out/stage
        cmake --build /out/build --target ovmx-images -- -j"$(nproc)"
        cmake --install /out/build --component ovmx-images
    '

# install_manifest_ovmx-images.txt (CMake writes this name -- not the plain
# install_manifest.txt -- because the install above named an explicit
# --component; see CMakeLists.txt's COMPONENT ovmx-images install loop,
# vms-88c) records every file that COMPONENT actually installed, as absolute
# paths INSIDE the container (under /out/stage/...). Since $STAGE3 is the
# same bind mount, the host path is the container path with the /out prefix
# swapped for $STAGE3 -- no separate translation table to maintain.
IMAGE_MANIFEST="$STAGE3/build/install_manifest_ovmx-images.txt"
[ -f "$IMAGE_MANIFEST" ] || fail "install_manifest_ovmx-images.txt missing after cmake --install --component ovmx-images -- the ovmx-images CMakeLists.txt COMPONENT install loop did not run"
[ -s "$IMAGE_MANIFEST" ] || fail "install_manifest_ovmx-images.txt is empty -- ovmx-images installed nothing"

IMAGE_NAMES=()
# `|| [ -n "$container_path" ]` is load-bearing: CMake's own
# install_manifest_*.txt (string(REPLACE ";" "\n" ...) + file(WRITE ...))
# ends WITHOUT a trailing newline after the last entry, so a plain
# `while read` silently drops that last line -- `read` returns nonzero on
# that final partial read even though it populated the variable, and a bare
# `while read ...; do` treats nonzero as loop-end before the body runs.
# Verified empirically: without this guard, PARTS.EXE (the last _OVMX_IMAGES_DEPS
# entry as of this writing) never made it into IMAGE_NAMES, $OUT_DIR, or the
# artifact manifest -- exactly the silent-drop class of bug INV-6 exists to
# kill, just self-inflicted instead of a fake success.
while IFS= read -r container_path || [ -n "$container_path" ]; do
    [ -n "$container_path" ] || continue
    name="$(basename "$container_path")"
    host_path="$STAGE3${container_path#/out}"
    [ -f "$host_path" ] || fail "image named in install_manifest_ovmx-images.txt is missing on disk: $name ($host_path)"
    cp "$host_path" "$OUT_DIR/$name"
    log "-> $OUT_DIR/$name"
    IMAGE_NAMES+=("$name")
done < "$IMAGE_MANIFEST"
[ "${#IMAGE_NAMES[@]}" -gt 0 ] || fail "no images copied from install_manifest_ovmx-images.txt"
log "ovmx-images aggregate: ${#IMAGE_NAMES[@]} images shipped (derived from install_manifest_ovmx-images.txt, not a hand list)"

# --- Shipped-artifact manifest (vms-88c): the DERIVED "what actually
# shipped" list, module artifacts first (fixed, kernel-module stages above)
# then every image name from the CMake install manifest, in the order CMake
# installed them. tools/cut-release.sh reads this file to build its
# VAX_ARTIFACT_ORDER array -- generated fresh every cut, never hand-
# duplicated, so it cannot drift from what was actually built.
{
    printf '%s\n' vms.kmod.o
    printf '%s\n' "${IMAGE_NAMES[@]}"
} > "$OUT_DIR/vax-artifact-manifest.txt"
log "-> $OUT_DIR/vax-artifact-manifest.txt ($(( ${#IMAGE_NAMES[@]} + 1 )) artifacts total)"

log "=== ALL VAX RELEASE ARTIFACTS BUILT: $OUT_DIR ==="
ls -la "$OUT_DIR"
