#!/bin/bash
# run_cluster_evac_ci.sh - the booted OVMX<->OVMX EVACUATION gate (rd vms-1e58,
# the CI half of the ci.6 release gate) and its negative control.
#
# Builds the 2-node genesis rig (tests/qemu/Dockerfile.cluster-genesis-2node:
# two QEMU guests, each loading the REAL vms.ko and reaching it through
# /dev/vms) and runs it in RIG_MODE=evac -- read the `evac` paragraph at the
# top of tests/qemu/run_cluster_genesis_2node.sh for what it does and what
# every verdict line is read from.
#
#   proof    the tree as it is. The rig must PASS: node A's queued standby is
#            GRANTED by its own executive after node B is killed -9, and it
#            continues the workload's sequence at seq+1 from the value block.
#
#   negctl   the SAME rig, built from a copy of the tree with
#            tests/cluster/host/host_defects.sh's dlm-departed-holder-locks-kept
#            applied (vms_lock_dlm_member_departed no longer releases the locks
#            the surviving master held for the departed system). The rig must
#            go RED, and for THAT reason only: the set-up (one resource on A's
#            master, B holding EX with its records committed, A's standby
#            queued, B evacuated) must still have happened -- verdicts (1)-(4)
#            green -- and the takeover (5) and continuation (6) must be what
#            failed. A run that is red because the cluster never formed, or B
#            never held the lock, proves nothing about the takeover.
#
# Docker is build/test tooling here (AGENTS.md "one runtime"): the runtime is
# the two QEMU guests' real executives. Everything this script creates is
# named with this run's own suffix and removed on exit.
#
# Usage: tests/qemu/run_cluster_evac_ci.sh {proof|negctl} [out-dir]
set -uo pipefail

MODE="${1:-}"
case "$MODE" in proof|negctl) ;; *)
	echo "usage: $0 {proof|negctl} [out-dir]" >&2; exit 2;;
esac
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="${2:-$(mktemp -d /tmp/ovmx-evac-$MODE.XXXXXX)}"
mkdir -p "$OUT"
TAG="ovmx-evac-rig:$MODE-$$"
NAME="ovmx-evac-$MODE-$$"
CTX=""
WALL="${EVAC_CI_WALL:-1500}"

cleanup() {
	docker kill "$NAME" >/dev/null 2>&1 || true
	docker rmi -f "$TAG" >/dev/null 2>&1 || true
	[ -n "$CTX" ] && rm -rf "$CTX"
}
trap cleanup EXIT INT TERM

command -v docker >/dev/null 2>&1 || { echo "FATAL: docker is required (build/test tooling)" >&2; exit 1; }

BUILD_DIR="$REPO_ROOT"
if [ "$MODE" = "negctl" ]; then
	# A private copy of exactly what the rig image is built from; the real
	# checkout is never touched.
	CTX=$(mktemp -d /tmp/ovmx-evac-ctx.XXXXXX)
	mkdir -p "$CTX/src/vmsfs" "$CTX/tests"
	cp -a "$REPO_ROOT/src/kernel-core" "$REPO_ROOT/src/kernel" "$CTX/src/"
	cp -a "$REPO_ROOT/src/vmsfs/include" "$REPO_ROOT/src/vmsfs/ods2" "$CTX/src/vmsfs/"
	cp -a "$REPO_ROOT/tests/qemu" "$CTX/tests/"
	bash "$REPO_ROOT/tests/cluster/host/host_defects.sh" apply \
		dlm-departed-holder-locks-kept "$CTX/src" \
		|| { echo "FATAL: could not inject dlm-departed-holder-locks-kept" >&2; exit 1; }
	rm -f "$CTX"/src/kernel-core/*.negctl-pristine
	grep "NEGCTL dlm-departed-holder-locks-kept" "$CTX/src/kernel-core/vms_lock.c" >/dev/null \
		|| { echo "FATAL: the defect is not in the copy the rig is built from" >&2; exit 1; }
	BUILD_DIR="$CTX"
fi

echo "=== building the 2-node rig ($MODE) ==="
docker build -t "$TAG" -f "$BUILD_DIR/tests/qemu/Dockerfile.cluster-genesis-2node" "$BUILD_DIR" \
	> "$OUT/build.log" 2>&1 || { tail -n 60 "$OUT/build.log"; echo "FATAL: rig image build failed" >&2; exit 1; }

KVM_ARGS=()
[ -e /dev/kvm ] && KVM_ARGS=(--device /dev/kvm)

echo "=== running RIG_MODE=evac ($MODE) ==="
timeout --kill-after=30 "$WALL" docker run --rm --name "$NAME" "${KVM_ARGS[@]}" \
	-e RIG_MODE=evac -v "$OUT:/out" "$TAG" > "$OUT/rig.log" 2>&1
RC=$?
tr -d '\r' < "$OUT/rig.log" > "$OUT/rig.txt"
sed -n '/=== rd vms-1e58: the EVACUATION/,$p' "$OUT/rig.txt"

failed() { grep "^  FAILED ($1)" "$OUT/rig.txt" >/dev/null; }

if [ "$MODE" = "proof" ]; then
	if [ "$RC" -eq 0 ] && grep "EVACUATION PROOF PASSED" "$OUT/rig.txt" >/dev/null; then
		echo "PASS: OVMX<->OVMX evacuation -- the standby was granted and continued the sequence"
		exit 0
	fi
	echo "--- rig log tail ---"; tail -n 120 "$OUT/rig.txt"
	echo "FAIL: the evacuation proof did not pass (rc=$RC)"
	exit 1
fi

# negctl: red, and red at the takeover only.
BAD=0
if [ "$RC" -eq 0 ] || grep "EVACUATION PROOF PASSED" "$OUT/rig.txt" >/dev/null; then
	echo "NEGATIVE CONTROL FAILED: with the departed system's locks kept, the rig still PASSED."
	BAD=1
fi
for n in 1 2 3 4 7 8; do
	if failed "$n"; then
		echo "NEGATIVE CONTROL INCONCLUSIVE: verdict ($n) is red -- the run failed before/around the"
		echo "takeover, so its redness says nothing about the takeover itself."
		BAD=1
	fi
done
for n in 5 6; do
	failed "$n" || { echo "NEGATIVE CONTROL FAILED: verdict ($n) did not go red."; BAD=1; }
done
if [ "$BAD" -ne 0 ]; then
	echo "--- rig log tail ---"; tail -n 120 "$OUT/rig.txt"
	exit 1
fi
echo "PASS: negative control -- with dlm-departed-holder-locks-kept the standby is never granted"
echo "      (verdicts 5 and 6 red, set-up verdicts 1-4 and 7-8 green): the gate can fail, for the right reason."
exit 0
