#!/bin/bash
# scratch driver (NOT a deliverable): apply one display-layer mutation, build
# the QEMU harness image, boot it, print the complete FAIL set, revert.
set -u
NAME="$1"; shift
echo "########## MUTATION $NAME ##########"
"$@" || { echo "MUTATION $NAME: sed anchor did not match"; exit 2; }
if git diff --quiet; then echo "MUTATION $NAME: NO CHANGE -- broken fixture"; exit 2; fi
podman build -f tests/qemu/Dockerfile -t ovmx-mut:$NAME . > /tmp/mut-$NAME-build.log 2>&1 \
  || { echo "MUTATION $NAME: image build failed"; tail -20 /tmp/mut-$NAME-build.log; git checkout -- .; exit 2; }
podman run --rm ovmx-mut:$NAME > /tmp/mut-$NAME.log 2>&1
echo "--- FAIL set ---"
tr -d '\r' < /tmp/mut-$NAME.log | grep -E "^  FAIL:|^=== SUITE .* rc=1|FINAL RESULTS"
echo "--- end ---"
podman rmi -f ovmx-mut:$NAME >/dev/null 2>&1
git checkout -- .
