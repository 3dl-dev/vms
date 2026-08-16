#!/bin/sh
# build-in-podman-alpha.sh — build + test IMGACT.EXE for Alpha (bead vms-e11,
# Alpha[A2], epic vms-8954).
#
# The Alpha analogue of build-in-podman-x86_64.sh, using the same
# ovmx-cross-alpha container tools/cross-alpha/ already provides for the
# freestanding libvmssys port (bead vms-a090): a Debian container with the
# alpha-linux-gnu cross toolchain and qemu-user's qemu-alpha (confirmed
# present: user-mode Alpha emulation, no full-system boot required to prove
# activation). Runs entirely containerized (Rule 9: build/test tooling only).
#
# Usage:  src/imgact/build-in-podman-alpha.sh
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)   # repo root
IMG=ovmx-cross-alpha

docker build -t "$IMG" "$REPO/tools/cross-alpha" >/dev/null

exec timeout 300 docker run --rm -v "$REPO":/src:ro -w /tmp "$IMG" sh -c '
	set -e
	cp -r /src /work
	sh /work/src/imgact/test/run_test_alpha.sh
'
