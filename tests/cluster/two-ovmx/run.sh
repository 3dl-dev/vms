#!/bin/bash
# run.sh [DURATION] -- build + run the two-OVMX-node SCS harness (rd vms-f3e).
#
# Runs on the workshop host (or any host with docker + the two caps). Builds the
# image from the repo tip, then runs the harness in its OWN container netns with
# CAP_NET_ADMIN + CAP_NET_RAW, dropping the pcap + logs + VERDICT into ./out/.
#
# Usage:
#   tests/cluster/two-ovmx/run.sh [DURATION_SECONDS]     # baseline (default 90)
#   SCSD_ENV="OVMX_JOIN_SEQ=1" tests/cluster/two-ovmx/run.sh 120   # diag run
#
# The container drops all Linux caps except the two it genuinely needs, so it is
# NOT --privileged and needs no /dev/net/tun. GitHub CI runners cannot grant
# NET_ADMIN/NET_RAW today, so this is a workshop-host / k3s-pod harness.
set -euo pipefail

DURATION="${1:-90}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../../.." && pwd)"
OUT_HOST="${OUT_HOST:-$HERE/out}"
IMAGE="${IMAGE:-ovmx-two-node}"
NAME="ovmx-two-node-$$"

mkdir -p "$OUT_HOST"
echo "[run] building $IMAGE from $REPO_ROOT"
docker build -f "$HERE/Dockerfile" -t "$IMAGE" "$REPO_ROOT"

echo "[run] running harness (duration=${DURATION}s) -> $OUT_HOST"
# Hard host-side timeout is a belt over the entrypoint's own guard.
# Docker's default cap set already carries NET_RAW (AF_PACKET) but NOT NET_ADMIN
# (bridge/veth create + the /sys multicast_snooping write); add it. We do NOT
# --cap-drop ALL: that also strips CAP_DAC_OVERRIDE, and container-root then
# cannot write the bind-mounted /out or the root-owned sysfs bridge knob. This
# is still an unprivileged run (no --privileged, no /dev/net/tun).
timeout $((DURATION + 120)) docker run --rm \
  --name "$NAME" \
  --cap-add NET_ADMIN --cap-add NET_RAW \
  -e DURATION="$DURATION" \
  -e SCSD_ENV="${SCSD_ENV:-}" \
  -e NODE_A="${NODE_A:-OVMXA}" -e SYSID_A="${SYSID_A:-1601}" \
  -e NODE_B="${NODE_B:-OVMXB}" -e SYSID_B="${SYSID_B:-1602}" \
  -v "$OUT_HOST:/out" \
  "$IMAGE" || {
    rc=$?
    echo "[run] container exited rc=$rc (verdict below reflects the actual join outcome)"
    docker kill "$NAME" 2>/dev/null || true
  }

echo
echo "[run] === VERDICT ==="
cat "$OUT_HOST/VERDICT.txt" 2>/dev/null || echo "(no VERDICT.txt produced)"
echo "[run] artifacts: $OUT_HOST"
