#!/bin/bash
# cut-release-vax.sh - build the VAX (netbsd-vax / elf32-vax) release artifact
# set for an OVMX release bundle (rd vms-ca5, epic vms-f10, "R2 release
# parity": docs/design-vax-mainstream-release.md).
#
# WHAT THIS IS. VAX is a first-class co-release platform, not a side-thread
# (docs/design-vax-mainstream-release.md). R1 (vms-9172, DONE) made a broken
# VAX build red a PR via fast per-PR cross-compile gates in ci.yml
# (netbsd-vax-vms-crosscompile, netbsd-vax-vmsfs-crosscompile, vmsdcl-netbsd-
# vax, ovmx-init-netbsd-vax/ovmx-boot-images-netbsd-vax, librarian-netbsd-
# vax). This script is R2: it reuses that SAME build recipe -- the exact
# tools/cross-vax/build-*.sh scripts those CI jobs already call, over the
# same pinned ovmx-cross-vax toolchain image and NetBSD syssrc -- to produce
# a real VAX artifact set and drop it into a release bundle. It does not
# reinvent the cross-compile; it drives it, the same way tools/cut-release.sh
# drives distro/Dockerfile.bootable rather than reinventing the x86_64 build.
#
# SCOPE (R2, explicitly NOT R3/R4). BUILD + artifact production only -- no
# SIMH boot, no custom MODULAR NetBSD/vax kernel build (that ~40-min kernel +
# boot proof is runtime parity, R3 vms-065, gated on the boot-to-DCL capstone
# vms-d59, still in flight). This script produces exactly what the FAST
# per-PR VAX gates already prove, for elf32-vax:
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
#   vmsfs.kmod         the loadable ODS-2 VFS module -- a genuine loadable
#                      kernel module (MODULE() metadata, no GOT32 relocs),
#                      built the same way the fast per-PR
#                      netbsd-vax-vmsfs-crosscompile gate proves it
#                      (build-vmsfs-mount-vax.sh). Its static mount helper
#                      (vmsfs_mount) ships alongside it.
#   STARTUP.EXE, PROVISION.EXE, DCL.EXE, JOB_CONTROL.EXE, LOGINOUT.EXE
#                      the full boot image set, ordinary dynamic NetBSD
#                      ELF32-vax executables activated by ld.elf_so
#                      (build-boot-images-vax.sh, Decision A / vms-42d).
#   LIBRARIAN.EXE      the object-library archiver (build-librarian-vax.sh).
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
# OVMX_VAX_RELEASE_NEGCTL=1 deliberately fails one stage (LIBRARIAN.EXE) --
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
    sed -n '2,63p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
[ "$NEGCTL" = "1" ] && log "OVMX_VAX_RELEASE_NEGCTL=1 -- WILL deliberately fail the LIBRARIAN.EXE stage (gate-wiring proof, never set this in a real cut)"

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
# Same pin the amd64 gate (tests/netbsd/netbsd_version.env) and the vax vmsfs
# harness (tests/lab-vax/run-vmsfs.sh) both use -- read from the ARCHIVED
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

# --- Stage 1/4: the OVMX executive module (vms.kmod.o, relocatable) --------
# build-vms-module-vax.sh does `rm -rf "$OUT"; mkdir -p "$OUT"` -- OUT is set
# to a SUBDIRECTORY of the bind mount (/out/build), not the mount point
# itself, because `rm -rf` on the mount point of a bind mount fails with
# "Device or resource busy" (Docker cannot unlink the mountpoint inode).
log "=== stage 1/4: OVMX executive module (vms.kmod.o) ==="
STAGE1="$WORK/vms-module"; mkdir -p "$STAGE1"
docker run --rm \
    -v "$SRC_DIR:/src" -w /src \
    -v "$NBSRC_CACHE:/nbsrc:ro" -e NBSRC=/nbsrc \
    -v "$STAGE1:/out" -e OUT=/out/build \
    "$TOOLCHAIN_IMAGE" tools/cross-vax/build-vms-module-vax.sh
[ -f "$STAGE1/build/vms.kmod.o" ] || fail "vms.kmod.o missing after cross-compile"
cp "$STAGE1/build/vms.kmod.o" "$OUT_DIR/vms.kmod.o"
log "-> $OUT_DIR/vms.kmod.o"

# --- Stage 2/4: the loadable ODS-2 VFS module (vmsfs.kmod) + mount helper --
log "=== stage 2/4: loadable ODS-2 VFS module (vmsfs.kmod) ==="
STAGE2="$WORK/vmsfs-mount"; mkdir -p "$STAGE2"
docker run --rm \
    -v "$SRC_DIR:/src" -w /src \
    -v "$NBSRC_CACHE:/nbsrc:ro" -e NBSRC=/nbsrc \
    -v "$STAGE2:/out" -e OUT=/out \
    "$TOOLCHAIN_IMAGE" sh tools/cross-vax/build-vmsfs-mount-vax.sh
[ -f "$STAGE2/vmsfs.kmod" ] || fail "vmsfs.kmod missing after cross-compile"
[ -f "$STAGE2/vmsfs_mount" ] || fail "vmsfs_mount (mount helper) missing after cross-compile"
cp "$STAGE2/vmsfs.kmod" "$OUT_DIR/vmsfs.kmod"
cp "$STAGE2/vmsfs_mount" "$OUT_DIR/vmsfs_mount"
log "-> $OUT_DIR/vmsfs.kmod, $OUT_DIR/vmsfs_mount"

# --- Stage 3/4: the full boot image set -------------------------------------
log "=== stage 3/4: boot image set (STARTUP/PROVISION/DCL/JOB_CONTROL/LOGINOUT) ==="
STAGE3="$WORK/boot-images"; mkdir -p "$STAGE3"
docker run --rm \
    -v "$SRC_DIR:/src" -w /src \
    -v "$STAGE3:/out" -e OUT_ROOT=/out \
    "$TOOLCHAIN_IMAGE" tools/cross-vax/build-boot-images-vax.sh
declare -A BOOT_IMAGE_SUBDIR=(
    [STARTUP.EXE]=ovmx-init
    [PROVISION.EXE]=provision
    [DCL.EXE]=vmsdcl
    [JOB_CONTROL.EXE]=job-control
    [LOGINOUT.EXE]=loginout
)
for img in STARTUP.EXE PROVISION.EXE DCL.EXE JOB_CONTROL.EXE LOGINOUT.EXE; do
    src="$STAGE3/${BOOT_IMAGE_SUBDIR[$img]}/$img"
    [ -f "$src" ] || fail "$img missing after cross-compile ($src)"
    cp "$src" "$OUT_DIR/$img"
    log "-> $OUT_DIR/$img"
done

# --- Stage 4/4: LIBRARIAN.EXE (also the OVMX_VAX_RELEASE_NEGCTL target) ----
log "=== stage 4/4: LIBRARIAN.EXE ==="
if [ "$NEGCTL" = "1" ]; then
    fail "OVMX_VAX_RELEASE_NEGCTL: deliberately failing the LIBRARIAN.EXE stage to prove the release-cut gate has teeth against a failed VAX build"
fi
# Same bind-mount-root `rm -rf "$OUT"` constraint as stage 1 -- OUT is a
# subdirectory of the mount, not the mount point itself.
STAGE4="$WORK/librarian"; mkdir -p "$STAGE4"
docker run --rm \
    -v "$SRC_DIR:/src" -w /src \
    -v "$STAGE4:/out" -e OUT=/out/build \
    "$TOOLCHAIN_IMAGE" tools/cross-vax/build-librarian-vax.sh
[ -f "$STAGE4/build/LIBRARIAN.EXE" ] || fail "LIBRARIAN.EXE missing after cross-compile"
cp "$STAGE4/build/LIBRARIAN.EXE" "$OUT_DIR/LIBRARIAN.EXE"
log "-> $OUT_DIR/LIBRARIAN.EXE"

log "=== ALL VAX RELEASE ARTIFACTS BUILT: $OUT_DIR ==="
ls -la "$OUT_DIR"
