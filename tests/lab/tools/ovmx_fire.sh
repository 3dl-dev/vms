#!/bin/bash
# [OVMX_FIRE_EXPVOTES=n] ovmx_fire.sh <pod> <artifacts-dir> <tag> <build-dir> <frame-type>... -- boot ONE
# OVMX node onto a lab pod's bridge, GATED (rd vms-ci.6).
#
# Refuses to start unless:
#   1. <artifacts-dir>/SOURCE_COMMIT names the commit <build-dir> was built from
#      (the gate must have checked the code that will run), and
#   2. the frame-conformance gate passes for every frame type this fire will
#      send to a real node (tests/cluster/conformance/run_gate.sh).
# Then it stages the artifacts in the pod and starts the node with a capture,
# console at /lab/k8s-labs/<pod>/logs/ovmxe.log (ovmx-lab-console reads it).
set -u
POD=${1:?pod}; ART=${2:?artifacts dir}; TAG=${3:?tag}; BUILD=${4:?build dir}; shift 4
[ $# -ge 1 ] || { echo "ovmx_fire.sh: name the frame types this fire sends" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)

SRC=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt")
BUILT=$(git -C "$SRC" rev-parse HEAD 2>/dev/null)
WANT=$(cat "$ART/SOURCE_COMMIT" 2>/dev/null | tr -d '[:space:]')
if [ -z "$BUILT" ] || [ "$BUILT" != "$WANT" ]; then
	echo "ovmx_fire.sh: REFUSED -- artifacts are $WANT, the gated build is $BUILT ($SRC)" >&2
	exit 1
fi
if [ -n "$(git -C "$SRC" status --porcelain --untracked-files=no)" ]; then
	echo "ovmx_fire.sh: REFUSED -- the gated tree $SRC has uncommitted changes" >&2
	exit 1
fi
if ! bash "$ROOT/tests/cluster/conformance/run_gate.sh" "$BUILD/bin" "$@"; then
	echo "ovmx_fire.sh: REFUSED -- the frame-conformance gate did not pass" >&2
	exit 1
fi

BASE=$(basename "$ART")
kubectl -n ovmx-lab cp "$ART" "$POD:/lab/$BASE" || exit 1
kubectl -n ovmx-lab cp "$HERE/ovmx_fire_node.sh" "$POD:/lab/run-evac/ovmx_fire_node.sh" || exit 1
kubectl -n ovmx-lab cp "$HERE/ovmx_evnode.sh" "$POD:/lab/run-evac/ovmx_evnode.sh" || exit 1
# OVMX_FIRE_EXPVOTES: OVMXE's EXPECTED_VOTES (default 3). 1 lets OVMXE found a
# cluster alone before any VAX boots (the OVMX-first order, rd vms-d52c).
exec kubectl -n ovmx-lab exec "$POD" -- bash /lab/run-evac/ovmx_fire_node.sh "/lab/$BASE" "$TAG" "${OVMX_FIRE_EXPVOTES:-3}"
