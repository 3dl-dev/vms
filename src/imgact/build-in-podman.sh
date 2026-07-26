#!/bin/sh
# build-in-podman.sh — build + test IMGACT.EXE in an Alpine (musl) container.
#
# Project rule: never install toolchains on the host. Alpine's `gcc` is a
# native musl toolchain, which is exactly what IMGACT.EXE (and OVMX QEMU-mode
# binaries) require. aarch64 host, no emulation.
#
# Usage:  src/imgact/build-in-podman.sh
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)   # repo root
IMG=docker.io/library/alpine:3.20

exec podman run --rm -v "$REPO":/src:Z -w /src "$IMG" sh -c '
	set -e
	apk add --no-cache gcc musl-dev binutils make >/dev/null 2>&1
	export CC=gcc
	sh /src/src/imgact/test/run_test.sh
'
