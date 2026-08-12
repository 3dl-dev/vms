#!/bin/bash
# run-local.sh -- build the lab-vax image and drive it locally, caching the
# slow NetBSD/vax install so it is paid exactly once (rd vms-0041).
#
#   tests/lab-vax/run-local.sh build     # build the container image only
#   tests/lab-vax/run-local.sh install   # install NetBSD/vax -> cached wd0.img
#   tests/lab-vax/run-local.sh smoke     # boot cached disk, assert uname
#   tests/lab-vax/run-local.sh negctl    # negative control (must fail-then-pass)
#   tests/lab-vax/run-local.sh all       # install (if needed) + smoke + negctl
#
# The disk cache follows the repo's .boot-cache/ convention (gitignored --
# multi-MB disk images are NEVER committed). Regenerate from scratch with:
#   rm -rf .boot-cache/lab-vax && tests/lab-vax/run-local.sh install
#
# Nothing is installed on the host: SIMH, anita, pexpect and NetBSD all live in
# the container / the mounted cache.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"
IMAGE="${IMAGE:-ovmx-vax-lab}"
CACHE_DIR="${CACHE_DIR:-${REPO}/.boot-cache/lab-vax}"
CMD="${1:-all}"

build() {
  echo "== building ${IMAGE} =="
  docker build -f "${HERE}/Dockerfile" -t "${IMAGE}" "${HERE}"
}

run_mode() {
  local mode="$1"; shift || true
  mkdir -p "${CACHE_DIR}"
  echo "== lab-vax: ${mode} (cache ${CACHE_DIR}) =="
  # No privilege needed: SIMH VAX drives its console over stdio, not a tap.
  docker run --rm \
    -v "${CACHE_DIR}:/cache" \
    "$@" \
    "${IMAGE}" "${mode}"
}

case "${CMD}" in
  build)   build ;;
  install) run_mode install ;;
  smoke)   run_mode smoke ;;
  negctl)  run_mode negctl ;;
  all)
    build
    run_mode install
    run_mode smoke
    run_mode negctl
    ;;
  *) echo "usage: $0 {build|install|smoke|negctl|all}" >&2; exit 2 ;;
esac
