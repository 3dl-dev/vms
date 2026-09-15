#!/bin/sh
# ensure-toolchain-image.sh (vms-495e) — make the alpha-dec-vms cross toolchain
# image ($IMG, default ovmx-cross-alpha-vms) present locally as CHEAPLY as
# possible, so alpha symbolize/capture/gate jobs skip the ~90-min GCC-14.2
# rebuild.
#
# Order of preference:
#   1. already present locally (a prior run, or CI's ghcr pull) -> done, 0s.
#   2. pull a content-hash-keyed copy from the LAN registry (30500) -> seconds.
#   3. cache miss -> build once from source, then PUSH it to 30500 so the NEXT
#      run (this worker or any other) pulls it. One machine eats the ~90 min; the
#      rest never do.
#
# Content-hash keying: the tag is derived from the toolchain-DEFINING inputs
# (Dockerfile + build-toolchain.sh + patches + the vendored binutils/gcc
# tarballs). A toolchain-source change bumps the hash -> correct rebuild, no
# stale-cache footgun. A CI-only edit elsewhere does not.
#
# LAN-only: GH cloud runners cannot reach 192.168.2.43:30500, so this populates
# from inside the k3s rail (run-on-rail --dind). CI keeps its own ghcr path
# (vms-e7c5); this is the manual/rail complement, and it NO-OPS when the image is
# already present (so it never conflicts with the ghcr flow).
#
# POSIX sh (dash-clean): build-joint-image.sh invokes this via sh; no bashisms
# (no `set -o pipefail`, no arrays, no `[[ ]]`).
set -u

REG=${OVMX_ALPHA_REG:-192.168.2.43:30500}
IMG=${IMG:-ovmx-cross-alpha-vms}
TC=$(CDPATH= cd "$(dirname "$0")" && pwd)   # tools/cross-alpha-vms (this script's dir)

# 1. already present locally?
if docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "ensure-toolchain: '$IMG' already present locally — no build, no pull"
    exit 0
fi

# content hash of the toolchain-defining inputs
HASH=$(
    {
        sha256sum "$TC/Dockerfile" "$TC/build-toolchain.sh" 2>/dev/null
        sha256sum "$TC"/patches/*.patch 2>/dev/null
        sha256sum "$TC"/binutils-*.tar.xz "$TC"/gcc-*.tar.xz 2>/dev/null
    } | sha256sum | cut -c1-16
)
TAG="$REG/$IMG:$HASH"
echo "ensure-toolchain: source-hash tag = $TAG"

# 2. pull the content-hash-keyed cached image from the LAN registry
if docker pull "$TAG" >/dev/null 2>&1; then
    docker tag "$TAG" "$IMG"
    echo "ensure-toolchain: PULLED cached $TAG -> '$IMG' (skipped the ~90-min GCC build)"
    exit 0
fi

# 3. cache miss -> build once, then push so the next run is cheap
echo "ensure-toolchain: cache miss ($TAG) -> building from source ONCE (~90 min), then populating the registry"
docker build -t "$IMG" "$TC" || { echo "ensure-toolchain: FAIL: docker build '$IMG'"; exit 1; }
docker tag "$IMG" "$TAG"
if docker push "$TAG" >/dev/null 2>&1; then
    echo "ensure-toolchain: PUSHED $TAG — future alpha jobs pull this instead of rebuilding"
else
    echo "ensure-toolchain: WARN: push to $TAG failed (image built locally; registry cache not populated this run)"
fi
exit 0
