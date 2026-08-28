#!/bin/bash
# run_musl_alpha_build.sh - end-to-end RUNG-1 proof for the OVMX alpha-dec-vms
# musl port (vms-960): build the cross toolchain image, then inside it fetch
# musl, overlay arch/alpha-dec-vms, and build+verify lib/libc.a.
#
# Build/oracle tooling, Rule-9-clean (see tools/cross-alpha-vms/README.md and
# musl-arch/README.md): nothing here runs inside the OVMX guest. All deps are
# containerized; the overlay is mounted read-only and the build runs in /tmp
# inside the container - the repo is never written to.
#
#   tools/cross-alpha-vms/run_musl_alpha_build.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
IMG=${IMG:-ovmx-cross-alpha-vms}

echo "== [1/2] build alpha-dec-vms cross toolchain image ($IMG) =="
docker build -t "$IMG" "$HERE"

echo "== [2/2] fetch musl + overlay alpha-dec-vms arch + build/verify libc.a =="
exec docker run --rm \
	-v "$HERE/musl-arch:/overlay:ro" \
	-e OVERLAY=/overlay \
	"$IMG" bash /overlay/build-musl.sh
