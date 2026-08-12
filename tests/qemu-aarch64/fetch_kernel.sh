#!/bin/bash
#
# fetch_kernel.sh - obtain a real arm64 Linux kernel Image for the boot proof.
#
#   fetch_kernel.sh <out-kernel-path>
#
# Downloads the Debian arm64 `linux-image-arm64` kernel package (via multiarch
# on the amd64 build host) and EXTRACTS the vmlinuz Image from it with
# `dpkg-deb -x` -- it never *installs* the package, so no arm64 maintainer
# scripts (update-initramfs &c.) run on the amd64 host. The Image is a genuine,
# Debian-provenanced arm64 kernel; qemu-system-aarch64 -machine virt boots it
# directly via -kernel. The resolved concrete version is printed for the record
# (this is BUILD/TEST tooling, CLAUDE.md Rule 9 -- not a shipped runtime).
set -euo pipefail

OUT="${1:?usage: fetch_kernel.sh <out-kernel-path>}"

dpkg --add-architecture arm64
apt-get update -qq

KPKG="$(apt-cache depends linux-image-arm64:arm64 | awk '/Depends:/{print $2; exit}')"
[ -n "$KPKG" ] || { echo "fetch_kernel.sh: could not resolve linux-image-arm64:arm64" >&2; exit 2; }
echo "fetch_kernel.sh: resolved arm64 kernel package: $KPKG"

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
( cd "$TMP" && { apt-get download "${KPKG}:arm64" || apt-get download "${KPKG}"; } )
dpkg-deb -x "$TMP"/*.deb "$TMP/x"

SRC="$(ls "$TMP"/x/boot/vmlinuz-* | head -1)"
[ -n "$SRC" ] || { echo "fetch_kernel.sh: no vmlinuz in $KPKG" >&2; exit 3; }
cp "$SRC" "$OUT"

file "$OUT" | grep -qi 'ARM64 boot executable Image' || {
    echo "fetch_kernel.sh: FAIL $OUT is not an ARM64 boot Image" >&2
    file "$OUT" >&2
    exit 4
}
echo "fetch_kernel.sh: $KPKG -> $OUT ($(file "$OUT" | cut -d: -f2-))"
