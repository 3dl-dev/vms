#!/bin/bash
# run-vmsfs-reject.sh - vms-1c7 regression proof: mounting a REAL-ODS2
# (DECFILE11B) volume through vmsfs.kmod on NetBSD/vax must be rejected with
# a clean EINVAL ("bad home block magic"), never a guest kernel panic
# ("vrelel: bad ref count").
#
# REUSES tests/lab-vax's proven substrate WITHOUT rebuilding it, the same way
# run-rawdisk.sh does: this wrapper does NOT install NetBSD and does NOT
# build the MODULAR kernel -- run-vmsfs.sh already produces the installed
# disk + MODULAR kernel + cross-built vmsfs.kmod/vmsfs_mount (cached under
# .boot-cache/lab-vax/{anita-work,vmsfs-artifacts}). This script's own job is
# ONLY to master a REAL-ODS2 test image (distinct from run-vmsfs.sh's
# bespoke-VMFS ovmx-ods2-vax.img -- see tests/qemu/mkimage_ods2_real.c) and
# boot the already-proven disk once more to run the rejection driver.
#
#   tests/lab-vax/run-vmsfs-reject.sh          # master image + boot + PROVE
#
# Prerequisite (run tests/lab-vax/run-vmsfs.sh first, or copy its cache):
#   anita-work/wd0.img                (installed NetBSD/vax, MODULAR kernel in)
#   vmsfs-artifacts/{vmsfs.kmod,vmsfs_mount,netbsd-OVMX}
#
# HARD timeout on the one SIMH invocation; container force-killed on timeout
# (CLAUDE.md memory note: a prior unbounded SIMH proof ran 1h43m).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"

NETBSD_VERSION="${NETBSD_VERSION:-10.1}"
SETS="${SETS:-kern-GENERIC,base,etc}"

CACHE_DIR="${CACHE_DIR:-${REPO}/.boot-cache/lab-vax}"
ARTIFACTS_SRC="${CACHE_DIR}/vmsfs-artifacts"          # vmsfs.kmod + vmsfs_mount (from run-vmsfs.sh)
REJECT_ARTIFACTS="${CACHE_DIR}/vmsfs-reject-artifacts"
REAL_ODS2_IMG="${REAL_ODS2_IMG:-${CACHE_DIR}/ovmx-ods2-real-vax.img}"
REAL_ODS2_SIZE_MB="${REAL_ODS2_SIZE_MB:-1}"

CROSS_IMAGE="${CROSS_IMAGE:-ovmx-cross-vax}"
LAB_IMAGE="${LAB_IMAGE:-ovmx-vax-lab}"

SESSION_TIMEOUT="${SESSION_TIMEOUT:-1200}"
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"

log() { echo "[run-vmsfs-reject] $*"; }
die() { echo "[run-vmsfs-reject] FATAL: $*" >&2; exit 1; }

ensure_images() {
  docker image inspect "${CROSS_IMAGE}" >/dev/null 2>&1 || die "${CROSS_IMAGE} not built -- run tests/lab-vax/run-vmsfs.sh once first"
  docker image inspect "${LAB_IMAGE}" >/dev/null 2>&1 || die "${LAB_IMAGE} not built -- run tests/lab-vax/run-vmsfs.sh once first"
}

ensure_prereqs() {
  [ -f "${CACHE_DIR}/anita-work/wd0.img" ] || die "no cached disk at ${CACHE_DIR}/anita-work/wd0.img -- run tests/lab-vax/run-vmsfs.sh first (or copy its cache)"
  if [ ! -f "${ARTIFACTS_SRC}/vmsfs.kmod" ] || [ ! -f "${ARTIFACTS_SRC}/vmsfs_mount" ]; then
    die "vmsfs.kmod/vmsfs_mount missing under ${ARTIFACTS_SRC} -- run tests/lab-vax/run-vmsfs.sh first"
  fi
  [ -f "${CACHE_DIR}/NetBSD-${NETBSD_VERSION}-vax.iso" ] || die "install ISO placeholder missing at ${CACHE_DIR}/NetBSD-${NETBSD_VERSION}-vax.iso (anita never opens it once wd0.img exists, but checks the path -- copy/link it from run-vmsfs.sh's cache)"
}

# master a REAL-ODS2 (DECFILE11B) volume -- the format vmsfs.kmod must REJECT.
master_real_ods2() {
  if [ -f "${REAL_ODS2_IMG}" ]; then
    log "real-ODS2 volume present -- NOT re-mastering (${REAL_ODS2_IMG})"; return 0; fi
  log "mastering a real-ODS2 (DECFILE11B) volume (tests/qemu/mkimage_ods2_real.c)"
  docker run --rm -v "${REPO}:/src:ro" -v "$(dirname "${REAL_ODS2_IMG}"):/out" \
    --entrypoint sh "${CROSS_IMAGE}" -c \
    "cc -O2 -Wall -Wextra -I /src/src/vmsfs/include -o /tmp/mkimage_ods2_real \
       /src/tests/qemu/mkimage_ods2_real.c /src/src/vmsfs/ods2/ods2_reader.c /src/src/vmsfs/ods2/ods2_writer.c && \
     /tmp/mkimage_ods2_real /out/$(basename "${REAL_ODS2_IMG}") ${REAL_ODS2_SIZE_MB}"
  [ -f "${REAL_ODS2_IMG}" ] || die "real-ODS2 mastering did not produce ${REAL_ODS2_IMG}"
  log "mastered real-ODS2 volume: $(ls -l "${REAL_ODS2_IMG}" | awk '{print $5}') bytes"
}

# stage the reject-driver's own copy of vmsfs.kmod/vmsfs_mount (unmodified).
stage_artifacts() {
  mkdir -p "${REJECT_ARTIFACTS}"
  cp -f "${ARTIFACTS_SRC}/vmsfs.kmod" "${REJECT_ARTIFACTS}/vmsfs.kmod"
  cp -f "${ARTIFACTS_SRC}/vmsfs_mount" "${REJECT_ARTIFACTS}/vmsfs_mount"
  chmod +x "${REJECT_ARTIFACTS}/vmsfs_mount"
}

run_session() {
  local cid="ovmx-vmsfs-reject-$$"; local rc=0
  set +e
  timeout --kill-after="${TIMEOUT_GRACE}" "${SESSION_TIMEOUT}" \
    docker run --rm --name "${cid}" --entrypoint python3 \
      -e NETBSD_VERSION="${NETBSD_VERSION}" -e "SETS=${SETS}" \
      -e OVMX_ARTIFACTS=/artifacts -e OVMX_NETBSD_DIR=/netbsd \
      -e OVMX_REAL_ODS2_IMG=/cache/"$(basename "${REAL_ODS2_IMG}")" \
      -v "${CACHE_DIR}:/cache" -v "${REJECT_ARTIFACTS}:/artifacts:ro" \
      -v "${REPO}/tests/netbsd:/netbsd:ro" -v "${REPO}/tests/lab-vax:/lab-vax:ro" \
      "${LAB_IMAGE}" /lab-vax/drive_vmsfs_reject_vax.py
  rc=$?; set -e
  docker kill "${cid}" >/dev/null 2>&1 || true
  return "${rc}"
}

ensure_images
ensure_prereqs
master_real_ods2
stage_artifacts

rc=0
run_session || rc=$?
if [ "${rc}" -eq 0 ]; then
  log "======================================================================"
  log "  VMSFS-REJECT-VAX PASSED (vms-1c7): real-ODS2 mount cleanly REJECTED,"
  log "  NO panic, guest kernel stayed alive"
  log "======================================================================"
  exit 0
fi
die "VMSFS-REJECT-VAX FAILED (driver exit=${rc}); see console output above -- exit 66 (HARNESS_ERROR) right after the mount attempt is CONSISTENT WITH THE PRE-FIX vms-1c7 PANIC (console hung/died)"
