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

# STAGED UNDER THE TAG, AND RE-VERIFIED IN THE POD (measured 2026-10-22).
#
# This used to stage at /lab/$(basename "$ART") and trust the SOURCE_COMMIT it
# had just read from the LOCAL directory. `kubectl cp <dir> <pod>:<dir>` NESTS
# when the destination already exists, so the SECOND fire whose artifacts
# directory had the same basename -- which every `gh run download` produces,
# they are all called `ovmx-boot-artifacts` -- landed at
# /lab/ovmx-boot-artifacts/ovmx-boot-artifacts/ and THE NODE BOOTED THE FIRST
# FIRE'S BITS while this script reported the gate green. Measured: fire q85f3
# ran a build whose SOURCE_COMMIT was the previous one, and the only tell was a
# missing line in a diagnostic.
#
# So the staging path carries the TAG (unique per fire by construction), and
# the commit is read back OUT OF THE POD afterwards. A fire now refuses rather
# than running the wrong executive.
BASE="art-$TAG"
kubectl -n ovmx-lab exec "$POD" -- rm -rf "/lab/$BASE" || exit 1
kubectl -n ovmx-lab cp "$ART" "$POD:/lab/$BASE" || exit 1
INPOD=$(kubectl -n ovmx-lab exec "$POD" -- sh -c "tr -d '[:space:]' < /lab/$BASE/SOURCE_COMMIT" 2>/dev/null)
if [ "$INPOD" != "$WANT" ]; then
	echo "ovmx_fire.sh: REFUSED -- the pod's staged artifacts are $INPOD, the gated build is $WANT" >&2
	exit 1
fi
kubectl -n ovmx-lab cp "$HERE/ovmx_fire_node.sh" "$POD:/lab/run-evac/ovmx_fire_node.sh" || exit 1
kubectl -n ovmx-lab cp "$HERE/ovmx_evnode.sh" "$POD:/lab/run-evac/ovmx_evnode.sh" || exit 1
# OVMX_FIRE_EXPVOTES: OVMXE's EXPECTED_VOTES (default 3). 1 lets OVMXE found a
# cluster alone before any VAX boots (the OVMX-first order, rd vms-d52c).
# OVMX_NODE / OVMX_SYSID / OVMX_MAC / OVMX_TAP / OVMX_NOCAP boot a second OVMX
# node in the same pod (the demo topology, rd vms-94a); see ovmx_fire_node.sh.
# RM_RESIDUE_FILL / LOCKDIRWT: SYSGEN values this fire wants the node booted
# with. They travel to the SYSBOOT> prompt (ovmx_evnode.sh), which persists them
# to OVMXVMSSYS.PAR before PID 1 reads the parameter set -- RA7's two fires
# differ in exactly this one variable (rd vms-6ad1).
exec kubectl -n ovmx-lab exec "$POD" -- env OVMX_NODE="${OVMX_NODE:-OVMXE}" \
	OVMX_SYSID="${OVMX_SYSID:-1030}" OVMX_MAC="${OVMX_MAC:-52:54:00:00:e5:01}" \
	OVMX_TAP="${OVMX_TAP:-tap3}" OVMX_NOCAP="${OVMX_NOCAP:-0}" \
	LOCKDIRWT="${LOCKDIRWT:-}" RM_RESIDUE_FILL="${RM_RESIDUE_FILL:-}" \
	bash /lab/run-evac/ovmx_fire_node.sh "/lab/$BASE" "$TAG" "${OVMX_FIRE_EXPVOTES:-3}"
