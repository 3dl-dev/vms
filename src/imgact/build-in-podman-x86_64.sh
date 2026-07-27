#!/bin/sh
# build-in-podman-x86_64.sh — build + test IMGACT.EXE for x86_64 (bead 913.11).
#
# The x86_64 analogue of build-in-podman.sh. Uses a Debian container providing
# the x86_64 cross toolchain (gcc-x86-64-linux-gnu) and qemu-user (qemu-x86_64),
# both native aarch64 binaries — no emulation is needed to BUILD, and RUNNING
# the activated x86_64 image is done under explicit qemu-x86_64. This keeps the
# project's containerized-deps rule (never install a toolchain on the host).
#
# The repo is mounted read-only and copied into the container before building,
# so no root-owned build artifacts land in the working tree.
#
# Usage:  src/imgact/build-in-podman-x86_64.sh
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)   # repo root
IMG=docker.io/library/debian:bookworm-slim

exec podman run --rm --platform linux/arm64 -v "$REPO":/src:ro -w /tmp "$IMG" sh -c '
	set -e
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq >/dev/null 2>&1
	apt-get install -y -qq make gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu \
		qemu-user >/dev/null 2>&1
	cp -r /src /work
	sh /work/src/imgact/test/run_test_x86_64.sh
'
