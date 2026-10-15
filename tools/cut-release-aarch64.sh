#!/bin/bash
# cut-release-aarch64.sh - build the aarch64 (OVMX/Linux on arm64) bootable
# release artifact set for an OVMX release bundle (rd vms-b58, epic vms-0ad0:
# aarch64 ships in V1.0).
#
# WHAT THIS IS. aarch64 is a first-class shipped arch, like x86_64: the SAME
# distro/Dockerfile.bootable pipeline tools/cut-release.sh drives for x86_64,
# built with TARGETARCH=arm64 (`--platform linux/arm64`). The kernel stage
# cross-compiles the pinned OVMX kernel + its in-tree, signed vms.ko for arm64
# (or builds natively on an arm64 host); every other stage runs as arm64 --
# natively on an arm64 host, under binfmt emulation elsewhere (correct, much
# slower). The set it ships is the x86_64 set, for arm64:
#
#   vmlinuz                      arm64 Image (the OVMX kernel)
#   initramfs-ovmx-slim.cpio.gz  vms.ko + STARTUP.EXE PID 1
#   ovmx-distrib.img             the pre-installed ODS-2 system disk
#   ovmx-os.kit                  the OS kit
#   ovmx-os.kit.manifest.txt     the kit's own listing
#
# and boots under `qemu-system-aarch64 -machine virt -cpu cortex-a57` (the
# release-acceptance gate boots exactly these files, OVMX_GUEST_ARCH=aarch64).
#
# It writes $OUT_DIR/aarch64-artifact-manifest.txt (the shipped basenames, in
# order) and $OUT_DIR/aarch64-cut-stamp (the commit + SOURCE_DATE_EPOCH this set
# was built from). tools/cut-release.sh reads the manifest to build its
# AARCH64_ARTIFACT_ORDER and, when handed a prebuilt set (--aarch64-dir), refuses
# it unless the stamp names the exact commit + clock being cut.
#
# GATE SEMANTICS. Runs under `set -e`; any stage failure exits nonzero, and
# tools/cut-release.sh calls it (or demands its prebuilt output) UNCONDITIONALLY,
# so a failed aarch64 build fails the cut. OVMX_AARCH64_RELEASE_NEGCTL=1 fails the
# build stage deliberately -- the proof the gate has teeth. Never set it in a
# real cut.
#
# Usage:
#   tools/cut-release-aarch64.sh --src-dir DIR --out-dir DIR \
#       --commit SHA --source-date-epoch N [--no-cache]
#
#   --src-dir DIR             the archived source tree being cut (never the
#                             live working repo)
#   --out-dir DIR             destination for the flat aarch64 artifact set
#   --commit SHA              the commit DIR was archived from (stamped)
#   --source-date-epoch N     the reproducible-build clock (passed to the build)
#   --no-cache                force a fresh, non-layer-cached build
#
# Env: OVMX_MODULE_SIGNING_KEY (as tools/cut-release.sh: a PEM fed as a BuildKit
# secret so module signatures are deterministic).

set -euo pipefail

SRC_DIR=""
OUT_DIR=""
COMMIT=""
SDE=""
NO_CACHE=0

usage() { sed -n '2,47p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        --src-dir) SRC_DIR="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --commit) COMMIT="$2"; shift 2 ;;
        --source-date-epoch) SDE="$2"; shift 2 ;;
        --no-cache) NO_CACHE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "cut-release-aarch64.sh: unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

log() { echo "cut-release-aarch64: $*"; }
fail() { echo "cut-release-aarch64: FATAL: $*" >&2; exit 1; }

[ -n "$SRC_DIR" ] || fail "--src-dir is required"
[ -n "$OUT_DIR" ] || fail "--out-dir is required"
[ -n "$COMMIT" ] || fail "--commit is required"
case "$SDE" in ''|*[!0-9]*) fail "--source-date-epoch must be a nonnegative integer, got: '$SDE'" ;; esac
[ -f "$SRC_DIR/distro/Dockerfile.bootable" ] || fail "no distro/Dockerfile.bootable in $SRC_DIR"
[ -f "$SRC_DIR/distro/kernel/ovmx-arm64.config" ] || fail "not an aarch64-capable OVMX tree (no distro/kernel/ovmx-arm64.config): $SRC_DIR"
command -v docker >/dev/null 2>&1 || fail "docker not found"

if [ "${OVMX_AARCH64_RELEASE_NEGCTL:-0}" = 1 ]; then
    fail "OVMX_AARCH64_RELEASE_NEGCTL: deliberately failing the aarch64 build stage to prove the release-cut gate has teeth against a failed aarch64 build"
fi

case "$(uname -m)" in
    aarch64|arm64) log "arm64 host: every stage builds natively" ;;
    *) log "$(uname -m) host: kernel cross-compiles; the userland stages run under binfmt arm64 emulation (slow)" ;;
esac

mkdir -p "$OUT_DIR"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/ovmx-cut-release-aarch64.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

ARGS=(build -f "$SRC_DIR/distro/Dockerfile.bootable" --platform linux/arm64
      --build-arg "SOURCE_DATE_EPOCH=$SDE"
      --output "type=local,dest=$WORK/out")
[ "$NO_CACHE" -eq 1 ] && ARGS+=(--no-cache)
if [ -n "${OVMX_MODULE_SIGNING_KEY:-}" ]; then
    [ -s "$OVMX_MODULE_SIGNING_KEY" ] || fail "OVMX_MODULE_SIGNING_KEY=$OVMX_MODULE_SIGNING_KEY is not a non-empty file"
    ARGS+=(--secret "id=ovmx_module_signing_key,src=$OVMX_MODULE_SIGNING_KEY")
fi
ARGS+=("$SRC_DIR")
log "building the arm64 bootable set (SOURCE_DATE_EPOCH=$SDE, commit $COMMIT)"
docker buildx "${ARGS[@]}"

NAMES=(vmlinuz initramfs-ovmx-slim.cpio.gz ovmx-distrib.img ovmx-os.kit ovmx-os.kit.manifest.txt)
for name in "${NAMES[@]}"; do
    [ -e "$WORK/out/boot/$name" ] || fail "expected arm64 build artifact missing: boot/$name"
    cp -L "$WORK/out/boot/$name" "$OUT_DIR/$name"
done

# The kernel must really be an arm64 Image ("ARM\x64" magic at 0x38,
# Documentation/arch/arm64/booting.rst) -- never a silently-x86 bzImage.
[ "$(dd if="$OUT_DIR/vmlinuz" bs=1 skip=56 count=3 2>/dev/null)" = "ARM" ] \
    || fail "aarch64/vmlinuz is not an arm64 Image (no ARM64 magic at 0x38)"
for n in DCL.EXE LOGINOUT.EXE STARTUP.COM; do
    grep -qF "$n" "$OUT_DIR/ovmx-os.kit.manifest.txt" || fail "aarch64 ovmx-os.kit.manifest.txt does not name $n"
done

printf '%s\n' "${NAMES[@]}" > "$OUT_DIR/aarch64-artifact-manifest.txt"
printf 'commit=%s\nsource_date_epoch=%s\n' "$COMMIT" "$SDE" > "$OUT_DIR/aarch64-cut-stamp"
log "=== aarch64 release artifacts built: $OUT_DIR ==="
ls -la "$OUT_DIR"
