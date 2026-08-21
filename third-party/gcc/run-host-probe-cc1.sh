#!/bin/sh
# run-host-probe-cc1.sh — container wrapper for host-probe-cc1.sh (bead
# vms-796, epic vms-da0 F2a). Mirrors src/imgact/build-in-podman.sh's shape
# (never install toolchains on the shared host); uses whichever of
# podman/docker is available.
#
# Alpine 3.20 (musl) with a real HOST C++ toolchain (g++) plus GMP/MPFR/MPC
# dev packages, since GCC's own build (cc1/cc1plus link) needs arbitrary-
# precision arithmetic support at BUILD time (see VENDOR-REV / the vms-0d2
# catalog note: "GMP/MPFR/MPC = static-link 7/52/18 into CC1.EXE").
#
# Build output is bind-mounted from a HOST work dir (default /tmp/gcc-cc1-
# hostprobe, per CLAUDE.md "build to /tmp") rather than left inside the
# --rm'd container's own filesystem, so a SEPARATE, quick, non-containerized
# pass (enumerate-cc1-walls.sh, using the host's own nm/c++filt/readelf —
# musl vs. glibc doesn't matter for static ELF inspection) can enumerate the
# full wall set afterward without re-running the long build.
#
# Usage:  third-party/gcc/run-host-probe-cc1.sh [host-work-dir]
# Must run from repo root.
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)   # repo root
IMG=docker.io/library/alpine:3.20
WORK=${1:-/tmp/gcc-cc1-hostprobe}

ENGINE=podman
command -v podman >/dev/null 2>&1 || ENGINE=docker
command -v "$ENGINE" >/dev/null 2>&1 || { echo "run-host-probe-cc1: neither podman nor docker found"; exit 1; }

mkdir -p "$WORK"

exec "$ENGINE" run --rm -v "$REPO":/src:Z -v "$WORK":/work:Z -w /src "$IMG" sh -c '
	set -e
	apk add --no-cache g++ gmp-dev mpfr-dev mpc1-dev make musl-dev binutils >/dev/null 2>&1
	sh /src/third-party/gcc/host-probe-cc1.sh /src/third-party/gcc/src /work
'
