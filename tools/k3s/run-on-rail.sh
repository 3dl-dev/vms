#!/usr/bin/env bash
# tools/k3s/run-on-rail.sh -- run a heavy OVMX build/test on the k3s rail.
#
# BUILD/TEST TOOLING ONLY (CLAUDE.md Rule 9). This offloads `cmake`/`ctest`/
# `tests/qemu/run_tests.sh` and the QEMU-KVM smoke from the shared workshop
# host onto cluster hardware (k3s-worker). It is NOT an OVMX runtime.
#
# Usage:
#   tools/k3s/run-on-rail.sh [--keep] [--name NAME] <git-ref> <command...>
#
#   <git-ref>    branch, tag, or full SHA of https://github.com/3dl-dev/vms
#   <command>    shell run in the repo root inside the pod; its exit code
#                becomes this script's exit code.
#
# Flags:
#   --keep       do not delete the Job when it finishes (for debugging)
#   --name NAME  base name for the Job (default: ovmx-ci)
#
# Examples:
#   tools/k3s/run-on-rail.sh main \
#     "cmake -B build -DBUILD_TESTS=ON -DBUILD_TOOLS=ON && \
#      cmake --build build -j\$(nproc) && cd build && ctest --output-on-failure"
#
#   tools/k3s/run-on-rail.sh main "bash tools/k3s/kvm-smoke.sh"
#
# Requires: kubectl (KUBECONFIG pointing at the rail), envsubst (gettext-base).

set -euo pipefail

# --- config -----------------------------------------------------------------
NS=ovmx-ci
REPO_URL="${OVMX_REPO_URL:-https://github.com/3dl-dev/vms}"
IMAGE="${OVMX_RAIL_IMAGE:-gpu-rail-registry.default.svc.cluster.local:5000/ovmx-builder:latest}"
DEADLINE="${OVMX_RAIL_DEADLINE:-3600}"    # activeDeadlineSeconds (wall cap)
REQ_CPU="${OVMX_RAIL_REQ_CPU:-4}"
REQ_MEM="${OVMX_RAIL_REQ_MEM:-8Gi}"
LIM_CPU="${OVMX_RAIL_LIM_CPU:-6}"
LIM_MEM="${OVMX_RAIL_LIM_MEM:-24Gi}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- args -------------------------------------------------------------------
KEEP=0
NAME_BASE=ovmx-ci
while [ $# -gt 0 ]; do
  case "$1" in
    --keep) KEEP=1; shift ;;
    --name) NAME_BASE="$2"; shift 2 ;;
    --)     shift; break ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    -*)     echo "run-on-rail: unknown flag $1" >&2; exit 2 ;;
    *)      break ;;
  esac
done

if [ $# -lt 2 ]; then
  echo "usage: run-on-rail.sh [--keep] [--name NAME] <git-ref> <command...>" >&2
  exit 2
fi

GIT_REF="$1"; shift
CMD="$*"

command -v kubectl >/dev/null || { echo "run-on-rail: kubectl not found" >&2; exit 127; }
command -v envsubst >/dev/null || { echo "run-on-rail: envsubst not found (apt install gettext-base)" >&2; exit 127; }

# --- ensure namespace + quota + limits exist --------------------------------
kubectl apply -f "$SCRIPT_DIR/namespace.yaml" >/dev/null

# --- render the Job manifest ------------------------------------------------
# The command is base64-encoded so arbitrary shell never touches the YAML.
OVMX_CMD_B64="$(printf '%s' "$CMD" | base64 | tr -d '\n')"
JOB_NAME="${NAME_BASE}-$(date +%s)-${RANDOM}"

export JOB_NAME IMAGE GIT_REF REPO_URL OVMX_CMD_B64 DEADLINE \
       REQ_CPU REQ_MEM LIM_CPU LIM_MEM
MANIFEST="$(envsubst \
  '$JOB_NAME $IMAGE $GIT_REF $REPO_URL $OVMX_CMD_B64 $DEADLINE $REQ_CPU $REQ_MEM $LIM_CPU $LIM_MEM' \
  < "$SCRIPT_DIR/job-template.yaml")"

cleanup() {
  if [ "$KEEP" -eq 1 ]; then
    echo "[run-on-rail] --keep: leaving Job $JOB_NAME (kubectl -n $NS delete job $JOB_NAME to remove)" >&2
  else
    kubectl -n "$NS" delete job "$JOB_NAME" --wait=false >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

echo "[run-on-rail] ref=$GIT_REF  job=$JOB_NAME  image=$IMAGE" >&2
echo "[run-on-rail] command: $CMD" >&2
printf '%s\n' "$MANIFEST" | kubectl apply -f - >/dev/null

# --- wait for the pod to exist and reach a streamable state -----------------
echo "[run-on-rail] waiting for pod to schedule on k3s-worker..." >&2
POD=""
for _ in $(seq 1 120); do
  POD="$(kubectl -n "$NS" get pods -l "ovmx.dev/job=$JOB_NAME" \
          -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || true)"
  [ -n "$POD" ] && break
  sleep 1
done
[ -n "$POD" ] || { echo "[run-on-rail] pod never appeared" >&2; exit 1; }

# Wait until the pod is out of Pending (Running or already terminated), so
# `logs -f` has something to stream. Surface scheduling failures early.
for _ in $(seq 1 300); do
  PHASE="$(kubectl -n "$NS" get pod "$POD" -o jsonpath='{.status.phase}' 2>/dev/null || true)"
  case "$PHASE" in
    Running|Succeeded|Failed) break ;;
  esac
  # Show pull/schedule trouble instead of hanging silently.
  WAITMSG="$(kubectl -n "$NS" get pod "$POD" \
    -o jsonpath='{.status.containerStatuses[0].state.waiting.reason}' 2>/dev/null || true)"
  case "$WAITMSG" in
    ErrImagePull|ImagePullBackOff|CreateContainerError|InvalidImageName)
      echo "[run-on-rail] pod stuck: $WAITMSG" >&2
      kubectl -n "$NS" describe pod "$POD" 2>&1 | sed -n '/Events:/,$p' >&2
      exit 1 ;;
  esac
  sleep 1
done

# --- stream logs ------------------------------------------------------------
echo "[run-on-rail] pod=$POD phase=$PHASE -- streaming logs:" >&2
echo "----------------------------------------------------------------------" >&2
kubectl -n "$NS" logs -f "$POD" 2>/dev/null || true

# --- wait for terminal state, then read the real exit code ------------------
kubectl -n "$NS" wait --for=condition=complete "job/$JOB_NAME" --timeout="${DEADLINE}s" >/dev/null 2>&1 &
W1=$!
kubectl -n "$NS" wait --for=condition=failed "job/$JOB_NAME" --timeout="${DEADLINE}s" >/dev/null 2>&1 &
W2=$!
wait -n "$W1" "$W2" 2>/dev/null || true
kill "$W1" "$W2" 2>/dev/null || true

# The container's terminated exitCode is the command's exit code (see template).
EXIT_CODE=""
for _ in $(seq 1 30); do
  EXIT_CODE="$(kubectl -n "$NS" get pod "$POD" \
    -o jsonpath='{.status.containerStatuses[0].state.terminated.exitCode}' 2>/dev/null || true)"
  [ -n "$EXIT_CODE" ] && break
  sleep 1
done

echo "----------------------------------------------------------------------" >&2
if [ -z "$EXIT_CODE" ]; then
  echo "[run-on-rail] could not read container exit code; treating as failure" >&2
  exit 1
fi
echo "[run-on-rail] pod=$POD node=$(kubectl -n "$NS" get pod "$POD" -o jsonpath='{.spec.nodeName}' 2>/dev/null) exit=$EXIT_CODE" >&2
exit "$EXIT_CODE"
